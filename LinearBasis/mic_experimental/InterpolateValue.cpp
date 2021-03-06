#ifdef HAVE_MIC
#include <assert.h>
#include <stdint.h>
#include <immintrin.h>
unsigned long long _rdtsc(void);
void _mm_delay_32(unsigned int val);
#include "tbb/parallel_reduce.h"
#include "tbb/blocked_range.h"
#else
#include "LinearBasis.h"
#endif

#ifdef HAVE_MIC

// Number of double precision elements in used AVX vector
#define AVX_VECTOR_SIZE 8

static __m512i sign_mask;
static __m512d double8_0_0_0_0_0_0_0_0;
static __m512d double8_1_1_1_1_1_1_1_1;

__attribute__((constructor)) void init_consts()
{
	sign_mask = _mm512_set_epi64(
		0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF,
		0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF);

	double8_0_0_0_0_0_0_0_0 = _mm512_setzero_pd();

	double8_1_1_1_1_1_1_1_1 = _mm512_set_pd(
		1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0);
}

// Adopted from C++ vector class library by Agner Fog
static inline __m512d _mm512_abs_pd(const __m512d x)
{
    return _mm512_castsi512_pd(_mm512_castpd_si512(x) & sign_mask);
}

using namespace tbb;

struct Interpolate
{
	const int dim, vdim, nno, Dof_choice;
	const double *x, *index, *surplus_t;

	__m512d x8;
	double value;

	Interpolate(const int dim_, const int vdim_, const int nno_,
		const int Dof_choice_, const double* x_,
		const double* index_, const double* surplus_t_) : 
		dim(dim_), vdim(vdim_), nno(nno_), Dof_choice(Dof_choice_),
		x(x_), index(index_), surplus_t(surplus_t_), value(0)
	{
#if defined(DEFERRED)
		if (DIM <= AVX_VECTOR_SIZE)
		{
			x8 = _mm512_load_pd(x);
		}
#endif
	}

	Interpolate(Interpolate& interp, split) :
		dim(interp.dim), vdim(interp.vdim), nno(interp.nno), Dof_choice(interp.Dof_choice),
		x(interp.x), index(interp.index), surplus_t(interp.surplus_t)
	{
		value = 0;
#if defined(DEFERRED)
		if (DIM <= AVX_VECTOR_SIZE)
		{
			x8 = _mm512_load_pd(x);
		}
#endif
	}

	void operator()(const blocked_range<int>& r)
	{
		for (int i = r.begin(); i != r.end(); i++)
		{
			int zero = 0;
			volatile __m512d temp = double8_1_1_1_1_1_1_1_1;
			for (int j = 0; j < DIM; j += AVX_VECTOR_SIZE)
			{
#if defined(DEFERRED)
				if (DIM > AVX_VECTOR_SIZE)
#endif
				{
					__m512d x8 = _mm512_load_pd(x + j);
				}

				// Read integer indexes, which are already converted to double,
				// because k1om can't load __m256i and convert anyway.
				volatile __m512d i8 = _mm512_load_pd((void*)&index[i * 2 * vdim + j]);
				volatile __m512d j8 = _mm512_load_pd((void*)&index[i * 2 * vdim + j + vdim]);

				volatile const __m512d xp = _mm512_sub_pd(double8_1_1_1_1_1_1_1_1,
					_mm512_abs_pd(_mm512_fmsub_pd (x8, i8, j8)));
				volatile __mmask8 d = _mm512_cmp_pd_mask(xp, double8_0_0_0_0_0_0_0_0, _MM_CMPINT_GT);
				if (d != 0xff)
				{
					zero = 1;
					break;
				}
				temp = _mm512_mul_pd(temp, xp);
			}
			if (zero) continue;

			// TODO GCC generates an unsupported vector instruction here.
			//value += _mm512_reduce_mul_pd(temp) * surplus_t[Dof_choice * nno + i];
			double reduction = _mm512_reduce_mul_pd(temp);
			__asm__ (
				  "fldl %[temp]\n\t"
				  "fmull %[surplus]\n\t"
				  "faddl %[value]\n\t"
				  "fstpl %[value]\n\t"
				: [value] "+m"(value)
				: [temp] "m"(reduction), [surplus] "m"(surplus_t[Dof_choice * nno + i]));
		}
	}

	void join(Interpolate& rhs)
	{
		// TODO GCC generates an unsupported vector instruction here.
		// value += rhs.value;
		__asm__ (
			  "fldl %[rhs_value]\n\t"
			  "faddl %[value]\n\t"
			  "fstpl %[value]\n\t"
			: [value] "+m"(value)
			: [rhs_value] "m"(rhs.value));
	}
};

#endif

static void interpolate(
	const int dim, const int nno,
	const int Dof_choice, const double* x,
	const double* index, const double* surplus_t, double* value_)
{
	assert(((size_t)x % (AVX_VECTOR_SIZE * sizeof(double)) == 0) && "x vector must be sufficiently memory-aligned");
	assert(((size_t)index % (AVX_VECTOR_SIZE * sizeof(double)) == 0) && "index vector must be sufficiently memory-aligned");
	assert(((size_t)surplus_t % (AVX_VECTOR_SIZE * sizeof(double)) == 0) && "surplus_t vector must be sufficiently memory-aligned");

	double value = 0.0;

	// Index arrays shall be padded to AVX_VECTOR_SIZE-element
	// boundary to keep up the required alignment.
	int vdim = dim / AVX_VECTOR_SIZE;
	if (dim % AVX_VECTOR_SIZE) vdim++;
	vdim *= AVX_VECTOR_SIZE;

#ifdef HAVE_MIC
	Interpolate interp(dim, vdim, nno, Dof_choice, x, index, surplus_t);
	parallel_reduce(blocked_range<int>(0, nno), interp);
	value = interp.value;
#else
	for (int i = 0; i < nno; i++)
	{
		int zero = 0;
		double temp = 1.0;
		for (int j = 0; j < DIM; j++)
		{
			double xp = LinearBasis(x[j], index[i * 2 * vdim + j],
				index[i * 2 * vdim + j + vdim]);
			if (xp <= 0.0)
			{
				zero = 1;
				break;
			}
			temp *= xp;
		}
		if (zero) continue;
		value += temp * surplus_t[Dof_choice * nno + i];
	}
#endif
	*value_ = value;
}

extern "C" void FUNCNAME(void* arg)
{
	typedef struct
	{
		int dim, nno, Dof_choice;
		double *x, *surplus_t, *value, *index;
	}
	Args;
	
	Args* args = (Args*)arg;

	interpolate(args->dim, args->nno, args->Dof_choice, args->x,
		args->index, args->surplus_t, args->value);
}

