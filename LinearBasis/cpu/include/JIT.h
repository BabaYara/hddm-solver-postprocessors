#ifndef JIT_H
#define JIT_H

#ifdef HAVE_RUNTIME_OPTIMIZATION

#include "Data.h"
#include "InterpolateKernel.h"

namespace cpu {

class Device;

typedef void (*InterpolateValueFunc)(
	Device* device,
	const int dim, const int nno,
	const int Dof_choice, const real* x,
	const Matrix<int>* index, const Matrix<real>* surplus,
	real* value);

typedef void (*InterpolateArrayFunc)(
	Device* device,
	const int dim, const int nno,
	const int Dof_choice_start, const int Dof_choice_end, const real* x,
	const Matrix<int>* index, const Matrix<real>* surplus,
	real* value);

typedef void (*InterpolateArrayManyStatelessFunc)(
	Device* device,
	const int dim, const int nno,
	const int Dof_choice_start, const int Dof_choice_end, const int count, const real* x,
	const Matrix<int>* index, const Matrix<real>* surplus,
	real* value);

typedef void (*InterpolateArrayManyMultistateFunc)(
	Device* device,
	const int dim, const int nno,
	const int Dof_choice_start, const int Dof_choice_end, const int count, const real* const* x,
	const Matrix<int>* index, const Matrix<real>* surplus,
	real** value);

typedef InterpolateKernel<InterpolateValueFunc> InterpolateValueKernel;
typedef InterpolateKernel<InterpolateArrayFunc> InterpolateArrayKernel;
typedef InterpolateKernel<InterpolateArrayManyStatelessFunc> InterpolateArrayManyStatelessKernel;
typedef InterpolateKernel<InterpolateArrayManyMultistateFunc> InterpolateArrayManyMultistateKernel;

class JIT
{
public :
	static InterpolateValueKernel& jitCompile(
		int dim, const std::string& funcnameTemplate, InterpolateValueFunc fallbackFunc);
	static InterpolateArrayKernel& jitCompile(
		int dim, const std::string& funcnameTemplate, InterpolateArrayFunc fallbackFunc);
	static InterpolateArrayManyStatelessKernel& jitCompile(
		int dim, int count, const std::string& funcnameTemplate, InterpolateArrayManyStatelessFunc fallbackFunc);
	static InterpolateArrayManyMultistateKernel& jitCompile(
		int dim, int count, const std::string& funcnameTemplate, InterpolateArrayManyMultistateFunc fallbackFunc);

	template<typename K, typename F>
	static K& jitCompile(int dim, int count, const std::string& funcnameTemplate, F fallbackFunc);
};

} // namespace cpu

#endif // HAVE_RUNTIME_OPTIMIZATION

#endif // JIT_H

