#include "LinearBasis.h"
#include "Data.h"

#include <algorithm> // min & max

#define CAT(kernel, name) name##_kernel
#define KERNEL(name) CAT(kernel, name)

// CUDA 8.0 introduces sm_60_atomic_functions.h with atomicAdd(double*, double)
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ < 600
inline __attribute__((always_inline)) __device__ double atomicAdd(double* address, double val)
{
	unsigned long long int* address_as_ull = (unsigned long long int*)address;
	unsigned long long int old = *address_as_ull, assumed;

	do
	{
		assumed = old;
		old = atomicCAS(address_as_ull, assumed,
			__double_as_longlong(val + __longlong_as_double(assumed)));
	}
	while (assumed != old);

	return __longlong_as_double(old);
}
#endif // __CUDA_ARCH__

using namespace NAMESPACE;
using namespace std;

class Device;

__global__ void KERNEL(FUNCNAME)(
	const int dim, const int nno, const int nnoPerBlock, const int DofPerNode, const int count, const double* const* x_,
	const int* nfreqs_, const XPS::Device* xps_, const int* szxps_, double** xpv_, const Chains::Device* chains_,
	const Matrix<double>::Device* surplus_, double** value_)
{
	for (int many = 0; many < COUNT; many++)
	{
		const double* x = x_[many];
		const int& nfreqs = nfreqs_[many];
		const XPS::Device& xps = xps_[many];
#if 0
		double* xpv = xpv_[blockIdx.x];
#else
		extern __shared__ double xpv[];
#endif
		const Chains::Device& chains = chains_[many];
		const Matrix<double>::Device& surplus = surplus_[many];
		double* value = value_[many];

		// Loop to calculate all unique xp values.
		for (int i = threadIdx.x, e = szxps_[many]; i < e; i += blockDim.x)
		{
			const Index<uint16_t>& index = xps(i);
			const uint32_t& j = index.index;
			double xp = LinearBasis(x[j], index.i, index.j);
			xpv[i] = fmax(0.0, xp);
		}

		__syncthreads();

#define szcache 4
		// Each thread hosts a part of blockDim.x-shared register cache
		// to accumulate nnoPerBlock intermediate additions.
		// blockDim.x -sharing is done due to limited number of registers
		// available per thread.
		double cache[szcache];
		for (int i = 0; i < szcache; i++)
			cache[i] = 0;
#undef szcache

		// Loop to calculate scaled surplus product.
		for (int i = blockIdx.x * nnoPerBlock, e = min(i + nnoPerBlock, NNO); i < e; i++)
		{
			double temp = 1.0;
			for (int ifreq = 0; ifreq < nfreqs; ifreq++)
			{
				// Early exit for shorter chains.
				int32_t idx = chains(i * nfreqs + ifreq);
				if (!idx) break;

				temp *= xpv[idx];
				if (!temp) goto next;
			}

			for (int Dof_choice = threadIdx.x, icache = 0; Dof_choice < DOF_PER_NODE; Dof_choice += blockDim.x, icache++)
				cache[icache] += temp * surplus(i, Dof_choice);
		
		next :

			continue;
		}

		for (int Dof_choice = threadIdx.x, icache = 0; Dof_choice < DOF_PER_NODE; Dof_choice += blockDim.x, icache++)
			atomicAdd(&value[Dof_choice], cache[icache]);
	}
}

namespace {

class InterpolateArrayManyMultistate
{
	Matrix<double>::Device xMatrixDev;
	Matrix<double>::Device valueMatrixDev;
	Matrix<double>::Device xpvMatrixDev;

public :

	int dim;
	int nno;
	int DofPerNode;
	int count;

	int szblock;
	int nnoPerBlock;
	int nblocks;

	double** xDev;
	double** valueDev;
	vector<int> szxps;
	int* szxpsDev;
	double** xpvDev;

	// Keep szxpv for further reference: its change
	// triggers xpv reallocation.
	int szxpv;

	cudaStream_t stream;

	InterpolateArrayManyMultistate(int dim, int nno, int DofPerNode, int count, const int* szxps_) :
		dim(dim), nno(nno), DofPerNode(DofPerNode), count(count),
		szblock(128), nnoPerBlock(16), nblocks(nno / nnoPerBlock + (nno % nnoPerBlock ? 1 : 0)),
		xDev(NULL), xMatrixDev(count, dim),
		valueDev(NULL), valueMatrixDev(count, dim),
		szxps(count), szxpsDev(NULL), szxpv(0), xpvDev(NULL)

	{
		CUDA_ERR_CHECK(cudaMalloc(&xDev, sizeof(double*) * count));
		vector<double*> x(count);
		for (int i = 0; i < count; i++)
			x[i] = xMatrixDev.getData(i, 0);
		CUDA_ERR_CHECK(cudaMemcpy(&xDev[0], &x[0], sizeof(double*) * count,
			cudaMemcpyHostToDevice));

		CUDA_ERR_CHECK(cudaMalloc(&valueDev, sizeof(double*) * count));
		vector<double*> value(count);
		for (int i = 0; i < count; i++)
			value[i] = valueMatrixDev.getData(i, 0);
		CUDA_ERR_CHECK(cudaMemcpy(&valueDev[0], &value[0], sizeof(double*) * count,
			cudaMemcpyHostToDevice));

		CUDA_ERR_CHECK(cudaMalloc(&szxpsDev, sizeof(int) * count));
		CUDA_ERR_CHECK(cudaMemcpy(&szxpsDev[0], &szxps_[0], sizeof(int) * count,
			cudaMemcpyHostToDevice));

		// Prepare the XPV buffer vector sized to max xps.size()
		// across all states. Individual buffer for each CUDA block.
		szxpv = 0;
		memcpy(&szxps[0], &szxps_[0], sizeof(int) * count);
		for (int many = 0; many < count; many++)
			szxpv = max(szxpv, szxps[many]);

		CUDA_ERR_CHECK(cudaMalloc(&xpvDev, sizeof(double*) * nblocks));
		vector<double*> xpv(nblocks);
		xpvMatrixDev.resize(nblocks, szxpv);
		for (int i = 0; i < nblocks; i++)
			xpv[i] = xpvMatrixDev.getData(i, 0);
		CUDA_ERR_CHECK(cudaMemcpy(&xpvDev[0], &xpv[0], sizeof(double*) * nblocks,
			cudaMemcpyHostToDevice));

		CUDA_ERR_CHECK(cudaStreamCreate(&stream));
	}

	template<class T>
	static inline void hashCombine(size_t& seed, const T& v)
	{
		std::hash<T> hasher;
		seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	}

	static size_t hash(int dim, int nno, int DofPerNode, int count)
	{
		size_t seed = 0;
		hashCombine(seed, dim);
		hashCombine(seed, nno);
		hashCombine(seed, DofPerNode);
		hashCombine(seed, count);
		return seed;
	}

	size_t hash() const
	{
		return InterpolateArrayManyMultistate::hash(dim, nno, DofPerNode, count);
	}
	
	void load(const double* const* x_, const int* szxps_)
	{
		for (int i = 0; i < count; i++)
			CUDA_ERR_CHECK(cudaMemcpy(xMatrixDev.getData(i, 0), x_[i], sizeof(double) * dim,
				cudaMemcpyHostToDevice));

		for (int i = 0; i < count; i++)
			CUDA_ERR_CHECK(cudaMemset(valueMatrixDev.getData(i, 0), 0, sizeof(double) * DOF_PER_NODE));

		// Copy szxps, if different.
		for (int i = 0; i < count; i++)
			if (szxps_[i] != szxps[i])
			{
				memcpy(&szxps[0], &szxps_[0], sizeof(int) * count);
				CUDA_ERR_CHECK(cudaMemcpy(&szxpsDev[0], &szxps_[0], sizeof(int) * count,
					cudaMemcpyHostToDevice));
				break;
			}

		int szxpvNew = 0;
		for (int many = 0; many < count; many++)
			szxpvNew = max(szxpvNew, szxps_[many]);
		
		// Reallocate xpv, if not enough space.
		if (szxpv < szxpvNew)
		{
			szxpv = szxpvNew;
		
			vector<double*> xpv(nblocks);
			xpvMatrixDev.resize(nblocks, szxpv);
			for (int i = 0; i < nblocks; i++)
				xpv[i] = xpvMatrixDev.getData(i, 0);
			CUDA_ERR_CHECK(cudaMemcpy(&xpvDev[0], &xpv[0], sizeof(double*) * nblocks,
				cudaMemcpyHostToDevice));
		}
	}

	void save(double** value_)
	{
		for (int i = 0; i < count; i++)
			CUDA_ERR_CHECK(cudaMemcpy(value_[i], valueMatrixDev.getData(i, 0), sizeof(double) * DOF_PER_NODE,
				cudaMemcpyDeviceToHost));
	}

	~InterpolateArrayManyMultistate()
	{
		CUDA_ERR_CHECK(cudaFree(xDev));
		CUDA_ERR_CHECK(cudaFree(valueDev));
		CUDA_ERR_CHECK(cudaFree(szxpsDev));
		CUDA_ERR_CHECK(cudaFree(xpvDev));

		CUDA_ERR_CHECK(cudaStreamDestroy(stream));
	}
};

unique_ptr<InterpolateArrayManyMultistate> interp;

} // namespace

extern "C" void FUNCNAME(
	Device* device,
	const int dim, const int nno, const int DofPerNode, const int count, const double* const* x_,
	const int* nfreqs_, const XPS::Device* xps_, const int* szxps_, const Chains::Device* chains_,
	const Matrix<double>::Device* surplus_, double** value_)
{
	bool rebuild = false;
	if (!interp.get())
		rebuild = true;
	else
		if (interp->hash() != InterpolateArrayManyMultistate::hash(dim, nno, DOF_PER_NODE, count))
			rebuild = true;
	
	if (rebuild)
		interp.reset(new InterpolateArrayManyMultistate(dim, nno, DOF_PER_NODE, count, szxps_));

	interp->load(x_, szxps_);

	// Launch the kernel.
#if 0
	KERNEL(FUNCNAME)<<<interp->nblocks, interp->szblock, interp->stream>>>(
#else
	KERNEL(FUNCNAME)<<<interp->nblocks, interp->szblock, interp->szxpv * sizeof(double), interp->stream>>>(
#endif
		dim, nno, interp->nnoPerBlock, DOF_PER_NODE, count, interp->xDev,
		nfreqs_, xps_, interp->szxpsDev, interp->xpvDev, chains_, surplus_, interp->valueDev);

	CUDA_ERR_CHECK(cudaStreamSynchronize(interp->stream));

	interp->save(value_);
}

