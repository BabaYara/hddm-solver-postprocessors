#define KERNEL_NAME InterpolateArrayManyMultistate_kernel_large_dim

#include "interpolator.h"
#include "Interpolate.h"
#include "LinearBasis.h"

static __global__ void KERNEL_NAME(
	const int dim, const int vdim, const int nno,
	const int Dof_choice_start, const int Dof_choice_end, const int count, const double* x_,
	const Matrix<int>::Device* index_, const Matrix<double>::Device* surplus_, double* value_)
{
	extern __shared__ double temps[];

	const int length = Dof_choice_end - Dof_choice_start + 1;

	for (int many = 0; many < count; many++)
	{
		const double* x = x_ + many * DIM;
		const Matrix<int>::Device& index = index_[many];
		const Matrix<double>::Device& surplus = surplus_[many];
		double* value = value_ + many * length;

		// The "i" is the index by nno, which could be either grid dimension X,
		// or partitioned between grid dimension X and block dimension Y.
		// In case of no partitioning, threadIdx.y is 0, and "i" falls back to
		// grid dimension X only.
		int i = blockIdx.x + threadIdx.y * blockDim.x;

		// Each thread is assigned with a "j" loop index.
		// If DIM is larger than AVX_VECTOR_SIZE, each thread is
		// assigned with multiple "j" loop indexes.
		double temp = 1.0;
		for (int j = threadIdx.x; (j < DIM) && (i < nno); j += blockDim.x)
		{
			double xp = LinearBasis(x[j], index(i, j), index(i, j + vdim));
			temp *= max(0.0, xp);
		}

		// Multiply all partial temps within a warp.
		temp = warpReduceMultiply(temp);
		
		// Gather temps from all participating warps corresponding to the single DIM
		// into a shared memory array.
		int lane = threadIdx.x % warpSize;
		int warpId = threadIdx.x / warpSize;
		int nwarps = blockDim.x / warpSize;
		if (lane == 0)
			temps[warpId + threadIdx.y * nwarps] = temp;

		// Wait for all partial reductions.
		__syncthreads();

		// We can only exit at this point, when all threads in block are synchronized.
		if (!temp) continue;
		if (i >= nno) continue;

		// Read from shared memory only if that warp existed.
		temp = (threadIdx.x < blockDim.x / warpSize) ? temps[lane + threadIdx.y * nwarps] : 1.0;

		// Final reduction within the first warp.
		if (warpId == 0)
		{
			temp = warpReduceMultiply(temp);
	
			// Store result into shared memory to broadcast across all warps.
			if (threadIdx.x == 0)
				temps[threadIdx.y * nwarps] = temp;
		}

		// Wait for the zero thread of the first warp to share temp value in shared memory.
		__syncthreads();

		// Load final reduction value from shared memory.
		temp = temps[threadIdx.y * nwarps];

		// Atomically add to the output value.
		// Uses double precision atomicAdd code above, since all
		// generations of GPUs before Pascal do not have double atomicAdd builtin.
		for (int j = Dof_choice_start + threadIdx.x; j <= Dof_choice_end; j += blockDim.x)
			atomicAdd(&value[j - Dof_choice_start], temp * surplus(i, j));
	}
}

extern "C" void FUNCNAME(
	Device* device,
	const int dim, const int nno,
	const int Dof_choice_start, const int Dof_choice_end, const int count, const double* const* x,
	const Matrix<int>::Device* index, const Matrix<double>::Device* surplus, double** value)
{
	// Configure kernel compute grid.
	int vdim = 1;
	dim3 blockDim(1, 1, 1);
	dim3 gridDim(1, 1, 1);
	int nwarps = 1;
	configureKernel(device, dim, nno, vdim, blockDim, gridDim, nwarps);
	
	double* dx;
	CUDA_ERR_CHECK(cudaMalloc(&dx, sizeof(double) * DIM * count));

	const int length = Dof_choice_end - Dof_choice_start + 1;
	double* dvalue;
	CUDA_ERR_CHECK(cudaMalloc(&dvalue, sizeof(double) * length * count));
	
	cudaStream_t stream;
	CUDA_ERR_CHECK(cudaStreamCreate(&stream));
	for (int i = 0; i < count; i++)
		CUDA_ERR_CHECK(cudaMemcpyAsync(dx + i * DIM, x[i], sizeof(double) * DIM, cudaMemcpyHostToDevice, stream));
	CUDA_ERR_CHECK(cudaMemsetAsync(dvalue, 0, sizeof(double) * length * count, stream));
	CUDA_ERR_CHECK(cudaStreamSynchronize(stream));
	CUDA_ERR_CHECK(cudaFuncSetSharedMemConfig(
		InterpolateArrayManyMultistate_kernel_large_dim, cudaSharedMemBankSizeEightByte));
	InterpolateArrayManyMultistate_kernel_large_dim<<<gridDim, blockDim, nwarps * sizeof(double), stream>>>(
		dim, vdim, nno, Dof_choice_start, Dof_choice_end, count, dx, index, surplus, dvalue);
	CUDA_ERR_CHECK(cudaGetLastError());
	CUDA_ERR_CHECK(cudaStreamSynchronize(stream));
	CUDA_ERR_CHECK(cudaDeviceSynchronize());
	for (int i = 0; i < count; i++)
		CUDA_ERR_CHECK(cudaMemcpyAsync(value[i], dvalue + i * length, sizeof(double) * length, cudaMemcpyDeviceToHost, stream));
	CUDA_ERR_CHECK(cudaStreamSynchronize(stream));
	CUDA_ERR_CHECK(cudaStreamDestroy(stream));

	CUDA_ERR_CHECK(cudaFree(dx));	
	CUDA_ERR_CHECK(cudaFree(dvalue));
}
