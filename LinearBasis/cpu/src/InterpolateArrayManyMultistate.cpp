#ifdef HAVE_AVX
#include <assert.h>
#include <stdint.h>
#include <x86intrin.h>
#endif

#include <algorithm> // min & max
#include <memory>
#include <mutex>
#include <utility> // pair

#include "LinearBasis.h"

#include "Data.h"

using namespace cpu;
using namespace std;

class Device;

static bool initialized = false;

struct Index
{
	int i, j;
	int rowind;

	Index() : i(0), j(0), rowind(0) { }
	
	Index(int i_, int j_, int rowind_) : i(i_), j(j_), rowind(rowind_) { }
};

static vector<vector<Index> > indexes_;

struct AVXIndex
{
	uint8_t i[AVX_VECTOR_SIZE], j[AVX_VECTOR_SIZE];
	uint16_t rowind[AVX_VECTOR_SIZE];
	
	AVXIndex()
	{
		memset(i, 0, sizeof(i));
		memset(j, 0, sizeof(j));
		memset(rowind, 0, sizeof(rowind));
	}
	
	bool isEmpty()
	{
		AVXIndex empty;
		if (memcmp(this, &empty, sizeof(AVXIndex)) == 0)
			return true;
		
		return false;
	}
};

class AVXIndexes: private vector<char, AlignedAllocator<AVXIndex> >
{
	int nnoMax;
	int dim;
	
	static const int szlength = 4 * sizeof(double);

	__attribute__((always_inline))
	static int nnoMaxAlign(int nnoMax_)
	{
		// Pad indexes rows to AVX_VECTOR_SIZE.
		if (nnoMax_ % AVX_VECTOR_SIZE)
			nnoMax_ += AVX_VECTOR_SIZE - nnoMax_ % AVX_VECTOR_SIZE;

		return nnoMax_ / AVX_VECTOR_SIZE;
	}
	
	__attribute__((always_inline))
	int& length(int j)
	{
		return reinterpret_cast<int*>(
			reinterpret_cast<char*>(&this->operator()(0, j)) - sizeof(int))[0];
	}

	__attribute__((always_inline))
	const int& length(int j) const
	{
		return reinterpret_cast<const int*>(
			reinterpret_cast<const char*>(&this->operator()(0, j)) - sizeof(int))[0];
	}
	
	__attribute__((always_inline))
	void setLength(int j, int length_)
	{
		length(j) = length_;
	}

public :

	AVXIndexes() :
		nnoMax(0), dim(0),
		vector<char, AlignedAllocator<AVXIndex> >()
	
	{ }

	AVXIndexes(int nnoMax_, int dim_) :
		nnoMax(nnoMaxAlign(nnoMax_)), dim(dim_),
		vector<char, AlignedAllocator<AVXIndex> >(
			dim_ * (nnoMaxAlign(nnoMax_) * sizeof(AVXIndex) + szlength))

	{ }
	
	void resize(int nnoMax_, int dim_)
	{
		nnoMax = nnoMaxAlign(nnoMax_);
		dim = dim_;

		vector<char, AlignedAllocator<AVXIndex> >::resize(
			dim_ * (nnoMaxAlign(nnoMax_) * sizeof(AVXIndex) + szlength));
	}
	
	__attribute__((always_inline))
	AVXIndex& operator()(int i, int j)
	{
		return *reinterpret_cast<AVXIndex*>(
			&vector<char, AlignedAllocator<AVXIndex> >::operator[]((j * nnoMax + i) * sizeof(AVXIndex) + (j + 1) * szlength));
	}

	__attribute__((always_inline))
	const AVXIndex& operator()(int i, int j) const
	{
		return *reinterpret_cast<const AVXIndex*>(
			&vector<char, AlignedAllocator<AVXIndex> >::operator[]((j * nnoMax + i) * sizeof(AVXIndex) + (j + 1) * szlength));
	}
	
	__attribute__((always_inline))
	int getLength(int j) const
	{
		return length(j);
	}
	
	void calculateLengths()
	{
		for (int j = 0; j < dim; j++)
		{
			int length = 0;
			for (int i = 0; i < nnoMax; i++)
			{
				AVXIndex& index = this->operator()(i, j);
				if (index.isEmpty())
					break;
				
				length++;
			}
			
			setLength(j, length);
		}
	}
};

static vector<AVXIndexes> avxinds_;

static vector<Matrix<double> > surplus_;

extern "C" void FUNCNAME(
	Device* device,
	const int dim, const int nno,
	const int Dof_choice_start, const int Dof_choice_end, const int count, const double* const* x_,
	const Matrix<int>* index_, const Matrix<double>* surplus__, double** value_)
{
	// Index arrays shall be padded to AVX_VECTOR_SIZE-element
	// boundary to keep up the required alignment.
	int vdim = dim / AVX_VECTOR_SIZE;
	if (dim % AVX_VECTOR_SIZE) vdim++;
	vdim *= AVX_VECTOR_SIZE;

	for (int many = 0; many < count; many++)
	{
		const double* x = x_[many];
		const Matrix<int>& index = index_[many];
		double* value = value_[many];

#ifdef HAVE_AVX
		assert(((size_t)x % (AVX_VECTOR_SIZE * sizeof(double)) == 0) && "x vector must be sufficiently memory-aligned");
#endif

		for (int b = Dof_choice_start, Dof_choice = b, e = Dof_choice_end; Dof_choice <= e; Dof_choice++)
			value[Dof_choice - b] = 0;

		if (!initialized)
		{
			static std::mutex mutex;
			std::lock_guard<std::mutex> lock(mutex);

			if (!initialized)
			{
				pair<int, int> zero = make_pair(0, 0);

				indexes_.resize(count);
				avxinds_.resize(count);
				surplus_.resize(count);
		
				for (int many = 0; many < count; many++)
				{
					vector<Index>& indexes = indexes_[many];
					Matrix<double>& surplus = surplus_[many];
					
					indexes.resize(dim);
					surplus.resize(nno, surplus__[many].dimx());

					// Convert (i, I) indexes matrix to sparse format.
					for (int i = 0; i < nno; i++)
						for (int j = 0; j < dim; j++)
						{
							// Get pair.
							pair<int, int> value = make_pair(index(i, j), index(i, j + vdim));
			
							// If both indexes are zeros, do nothing.
							if (value == zero)
								continue;
			
							// Find free position for non-zero pair.
							bool foundPosition = false;
							for (int irow = 0, nrows = indexes.size() / dim; irow < nrows; irow++)
							{
								Index& index = indexes[irow * dim + j];
								if (make_pair(index.i, index.j) == zero)
								{
									index.i = value.first;
									index.j = value.second;
									index.rowind = i;

									foundPosition = true;
									break;
								}
							}
							if (!foundPosition)
							{
								// Add new free row.
								indexes.resize(indexes.size() + dim);

								Index& index = indexes[indexes.size() - dim + j];
				
								index.i = value.first;
								index.j = value.second;
								index.rowind = i;
							}
						}
					
#if 0
					cout << "Before reordering: " << endl;
					for (int i = 0, order = 0, e = indexes.size() / dim; i < e; i++)
					{
						cout << "row " << i << " : ";
						for (int j = 0; j < dim; j++)
						{
							Index& index = indexes[i * dim + j];

							cout << "{[" << index.i << "," << index.j << "]," << index.rowind << "} ";
						}
						cout << endl;
					}
#endif
					
					// Reorder indexes.
					map<int, int> mapping;
					for (int j = 0, order = 0; j < dim; j++)
						for (int i = 0, e = indexes.size() / dim; i < e; i++)
						{
							Index& index = indexes[i * dim + j];

							if ((index.i == 0) && (index.j == 0)) continue;
							
							if (mapping.find(index.rowind) == mapping.end())
							{
								mapping[index.rowind] = order;
								index.rowind = order;
								order++;
							}
							else
								index.rowind = mapping[index.rowind];
						}
					
					// Pad indexes rows to AVX_VECTOR_SIZE.
					if ((indexes.size() / dim) % AVX_VECTOR_SIZE)
						indexes.resize(indexes.size() +
							dim * (AVX_VECTOR_SIZE - (indexes.size() / dim) % AVX_VECTOR_SIZE));
					
					// Do not forget to reorder unseen indexes.
					for (int i = 0, last = mapping.size(); i < nno; i++)
						if (mapping.find(i) == mapping.end())
							mapping[i] = last++;

					// Reorder surpluses.
					for (map<int, int>::iterator i = mapping.begin(), e = mapping.end(); i != e; i++)
					{
						int oldind = i->first;
						int newind = i->second;
						
						memcpy(&surplus(newind, 0), &(surplus__[many](oldind, 0)), surplus.dimx() * sizeof(double));
					}

#if 0
					cout << endl << "After reordering: " << endl;
					for (int i = 0, order = 0, e = indexes.size() / dim; i < e; i++)
					{
						cout << "row " << i << " : ";
						for (int j = 0; j < dim; j++)
						{
							Index& index = indexes[i * dim + j];

							cout << "{[" << index.i << "," << index.j << "]," << index.rowind << "} ";
						}
						cout << endl;
					}
#endif

					AVXIndexes& avxinds = avxinds_[many];
					avxinds.resize(nno, dim);

					for (int j = 0, iavx = 0, length = indexes.size() / dim / AVX_VECTOR_SIZE; j < dim; j++)
					{
						for (int i = 0; i < length; i++)
						{
							AVXIndex& index = avxinds(i, j);
							for (int k = 0; k < AVX_VECTOR_SIZE; k++)
							{
								int idx = (i * AVX_VECTOR_SIZE + k) * dim + j;
								index.i[k] = indexes[idx].i;
								index.j[k] = indexes[idx].j;
								index.rowind[k] = indexes[idx].rowind;
							}
						}
					}
					
					avxinds.calculateLengths();
				}
			
				initialized = true;
			}
		}

		const Matrix<double>& surplus = surplus_[many];

		AVXIndexes& avxinds = avxinds_[many];

#ifdef HAVE_AVX
		const __m256d double4_0_0_0_0 = _mm256_setzero_pd();
		const __m256d double4_1_1_1_1 = _mm256_set1_pd(1.0);
		const __m256d sign_mask = _mm256_set1_pd(-0.);
		const __m128i int4_0_0_0_0 = _mm_setzero_si128();

		// Loop to calculate temps.
		vector<double> temps(nno, 1.0);
		for (int j = 0; j < dim; j++)
		{
			const __m256d x64 = _mm256_set1_pd(x[j]);
		
			for (int i = 0, e = avxinds.getLength(j); i < e; i++)
			{
				AVXIndex& index = avxinds(i, j);

				const __m128i ij8 = _mm_load_si128(reinterpret_cast<const __m128i*>(&index));
				const __m128i i16 = _mm_unpacklo_epi8(ij8, int4_0_0_0_0);
				const __m128i j16 = _mm_unpackhi_epi8(ij8, int4_0_0_0_0);

				const __m128i i32lo = _mm_unpacklo_epi16(i16, int4_0_0_0_0);
				const __m128i i32hi = _mm_unpackhi_epi16(i16, int4_0_0_0_0);

				const __m128i j32lo = _mm_unpacklo_epi16(j16, int4_0_0_0_0);
				const __m128i j32hi = _mm_unpackhi_epi16(j16, int4_0_0_0_0);

				__m256d xp64lo = _mm256_sub_pd(double4_1_1_1_1, _mm256_andnot_pd(sign_mask,
					_mm256_sub_pd(_mm256_mul_pd(x64, _mm256_cvtepi32_pd(i32lo)), _mm256_cvtepi32_pd(j32lo))));

				const __m256d mask64lo = _mm256_cmp_pd(xp64lo, double4_0_0_0_0, _CMP_GT_OQ);
				xp64lo = _mm256_blendv_pd(double4_0_0_0_0, xp64lo, mask64lo);

				__m256d xp64hi = _mm256_sub_pd(double4_1_1_1_1, _mm256_andnot_pd(sign_mask,
					_mm256_sub_pd(_mm256_mul_pd(x64, _mm256_cvtepi32_pd(i32hi)), _mm256_cvtepi32_pd(j32hi))));

				const __m256d mask64hi = _mm256_cmp_pd(xp64hi, double4_0_0_0_0, _CMP_GT_OQ);
				xp64hi = _mm256_blendv_pd(double4_0_0_0_0, xp64hi, mask64hi);

				double xp[AVX_VECTOR_SIZE] __attribute__((aligned(AVX_VECTOR_SIZE * sizeof(double))));
				_mm256_store_pd(&xp[0], xp64lo);
				_mm256_store_pd(&xp[0 + sizeof(xp64lo) / sizeof(double)], xp64hi);
				for (int k = 0; k < AVX_VECTOR_SIZE; k++)
				{
					uint16_t& rowind = index.rowind[k];
			
					// This can be done scalar only.
					temps[rowind] *= xp[k];
				}
			}			
		}

		// Loop to calculate values.
		for (int i = 0; i < nno; i++)
		{
			double temp = temps[i];
			if (!temp) continue;

			const __m256d temp64 = _mm256_set1_pd(temp);

			for (int b = Dof_choice_start, Dof_choice = b, e = Dof_choice_end; Dof_choice <= e;
				Dof_choice += sizeof(temp64) / sizeof(double))
			{
				const __m256d surplus64 = _mm256_load_pd(&surplus(i, Dof_choice));
				__m256d value64 = _mm256_load_pd(&value[Dof_choice - b]);
				
				// XXX Can be FMA here, if AVX2 is available
				value64 = _mm256_add_pd(value64, _mm256_mul_pd(temp64, surplus64));

				_mm256_store_pd(&value[Dof_choice - b], value64);
			}
		}
#else
		// Loop to calculate temps.
		vector<double> temps(nno, 1.0);
		for (int j = 0; j < dim; j++)
		{
			double xx = x[j];
		
			for (int i = 0, e = avxinds.getLength(j); i < e; i++)
			{
				AVXIndex& index = avxinds(i, j);

				for (int k = 0; k < AVX_VECTOR_SIZE; k++)
				{
					uint8_t& ind_i = index.i[k];
					uint8_t& ind_j = index.j[k];
					uint16_t& rowind = index.rowind[k];

					double xp = LinearBasis(xx, ind_i, ind_j);

					xp = fmax(0.0, xp);
			
					temps[rowind] *= xp;
				}
			}			
		}

		// Loop to calculate values.
		for (int i = 0; i < nno; i++)
		{
			double temp = temps[i];
			if (!temp) continue;

			for (int b = Dof_choice_start, Dof_choice = b, e = Dof_choice_end; Dof_choice <= e; Dof_choice++)
				value[Dof_choice - b] += temp * surplus(i, Dof_choice);
		}		
#endif
	}
}

