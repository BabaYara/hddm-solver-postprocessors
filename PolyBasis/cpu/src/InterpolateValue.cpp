#ifdef HAVE_AVX
#include <assert.h>
#include <stdint.h>
#include <x86intrin.h>
#else
#include "LinearBasis.h"
#endif

#include "Data.h"

#include <stdio.h>
#include <stdlib.h>

#define STR(funcname) #funcname

extern "C" void FUNCNAME(
	const int dim, const int nno,
	const int Dof_choice, const double* x,
	const Matrix<int>& index, const Matrix<double>& surplus, double* value_)
{
	printf("%s is not implemented\n", STR(FUNCNAME));
	abort();

#ifdef HAVE_AVX
	assert(((size_t)x % (AVX_VECTOR_SIZE * sizeof(double)) == 0) && "x vector must be sufficiently memory-aligned");
#endif

	double value = 0.0;

	// Index arrays shall be padded to AVX_VECTOR_SIZE-element
	// boundary to keep up the required alignment.
	int vdim = dim / AVX_VECTOR_SIZE;
	if (dim % AVX_VECTOR_SIZE) vdim++;
	vdim *= AVX_VECTOR_SIZE;
#ifdef HAVE_AVX
	const __m256d double4_0_0_0_0 = _mm256_setzero_pd();
	const __m256d double4_1_1_1_1 = _mm256_set1_pd(1.0);
	const __m256d sign_mask = _mm256_set1_pd(-0.);

	__m256d x4;
#if defined(DEFERRED)
	if (DIM <= AVX_VECTOR_SIZE)
		x4 = _mm256_load_pd(x);
#endif
	for (int i = 0; i < nno; i++)
	{
		__m256d temp = double4_1_1_1_1;
		for (int j = 0; j < DIM; j += AVX_VECTOR_SIZE)
		{
#if defined(DEFERRED)
			if (DIM > AVX_VECTOR_SIZE)
#endif
			{
				x4 = _mm256_load_pd(x + j);
			}

			__m128i i4 = _mm_load_si128(reinterpret_cast<const __m128i*>(&index(i, j)));
			__m128i j4 = _mm_load_si128(reinterpret_cast<const __m128i*>(&index(i, j + vdim)));
			const __m256d xp = _mm256_sub_pd(double4_1_1_1_1, _mm256_andnot_pd(sign_mask,
				_mm256_sub_pd(_mm256_mul_pd(x4, _mm256_cvtepi32_pd(i4)), _mm256_cvtepi32_pd(j4))));
			const __m256d d = _mm256_cmp_pd(xp, double4_0_0_0_0, _CMP_GT_OQ);
			if (_mm256_movemask_pd(d) != (int)0xf)
				goto zero;
			temp = _mm256_mul_pd(temp, xp);
		}
		
		{
			const __m128d pairwise_mul = _mm_mul_pd(_mm256_castpd256_pd128(temp), _mm256_extractf128_pd(temp, 1));
			value += _mm_cvtsd_f64(_mm_mul_pd(pairwise_mul, (__m128d)_mm_movehl_ps((__m128)pairwise_mul, (__m128)pairwise_mul))) *
				surplus(i, Dof_choice);
		}

		zero : continue;
	}
#else
	for (int i = 0; i < nno; i++)
	{
		double temp = 1.0;
		for (int j = 0; j < DIM; j++)
		{
			double xp = LinearBasis(x[j], index(i, j), index(i, j + vdim));
			if (xp <= 0.0)
				goto zero;
			temp *= xp;
		}
		value += temp * surplus(i, Dof_choice);

		zero : continue;
	}
#endif
	*value_ = value;
}

