#ifndef MATRIX_H
#define MATRIX_H

#include "DataContainer.h"

namespace cuda {

namespace Matrix {

namespace Host {

// Host memory matrix with all rows aligned
template<typename T, typename VectorType = std::vector<T, AlignedAllocator::Host<T> > >
class Dense
{
	VectorType data;
	int dimY, dimX, dimX_aligned;

public :
	Dense() : data(VectorType()), dimY(0), dimX(0) { }

	Dense(int dimY_, int dimX_) : data(VectorType()), dimY(dimY_), dimX(dimX_)
	{
		dimX_aligned = dimX_;
		if (dimX_ % AVX_VECTOR_SIZE)
			dimX_aligned = dimX + AVX_VECTOR_SIZE - dimX_ % AVX_VECTOR_SIZE;
		data.resize(dimY_ * dimX_aligned);
	}

	inline __attribute__((always_inline)) T* getData() { return &data[0]; }
	
	inline __attribute__((always_inline)) T& operator()(int y, int x)
	{
		assert(x < dimX);
		assert(y < dimY);
		int index = x + dimX_aligned * y;
		assert(index < data.size());
		return data[index];
	}

	inline __attribute__((always_inline)) const T& operator()(int y, int x) const
	{
		assert(x < dimX);
		assert(y < dimY);
		int index = x + dimX_aligned * y;
		assert(index < data.size());
		return data[index];
	}

	inline __attribute__((always_inline)) int dimy() { return dimY; }

	inline __attribute__((always_inline)) int dimx() { return dimX; }
		
	inline __attribute__((always_inline)) void resize(int dimY_, int dimX_)
	{
		dimY = dimY_; dimX = dimX_;
		dimX_aligned = dimX_;
		if (dimX_ % AVX_VECTOR_SIZE)
			dimX_aligned = dimX + AVX_VECTOR_SIZE - dimX_ % AVX_VECTOR_SIZE;
		data.resize(dimY_ * dimX_aligned);
	}
	
	inline __attribute__((always_inline)) void fill(T value)
	{
		std::fill(data.begin(), data.end(), value);
	}
};

namespace Sparse {

// Host memory sparse matrix in CSR format
template<typename TValue, typename TIndex, template<typename, typename> class TVector = std::vector, template<typename> class TAllocator = AlignedAllocator::Host>
class CSR
{
	TVector<TValue, TAllocator<TValue> > a_;
	TVector<TIndex, TAllocator<TIndex> > ia_, ja_;
	int dimY, dimX, nnZ;

public :
	CSR() :
		a_(TVector<TValue, TAllocator<TValue> >()),
		ia_(TVector<TIndex, TAllocator<TIndex> >()),
		ja_(TVector<TIndex, TAllocator<TIndex> >()),
		dimY(0), dimX(0), nnZ(0)
	{ }

	CSR(int dimY_, int dimX_, int nnz_) :
		a_(TVector<TValue, TAllocator<TValue> >()),
		ia_(TVector<TIndex, TAllocator<TIndex> >()),
		ja_(TVector<TIndex, TAllocator<TIndex> >()),
		dimY(dimY_), dimX(dimX_), nnZ(nnz_)
	{
		a_.resize(nnZ);
		ia_.resize(dimY + 1);
		ja_.resize(nnZ);
	}

	inline __attribute__((always_inline)) TValue& a(int i)
	{
		assert(i < nnZ);
		return a_[i];
	}

	inline __attribute__((always_inline)) const TValue& a(int i) const
	{
		assert(i < nnZ);
		return a_[i];
	}
	
	inline __attribute__((always_inline)) TIndex& ia(int i)
	{
		assert(i < dimY + 1);
		return ia_[i];
	}

	inline __attribute__((always_inline)) const TIndex& ia(int i) const
	{
		assert(i < dimY + 1);
		return ia_[i];
	}
	
	inline __attribute__((always_inline)) TIndex& ja(int i)
	{
		assert(i < nnZ);
		return ja_[i];
	}

	inline __attribute__((always_inline)) const TIndex& ja(int i) const
	{
		assert(i < nnZ);
		return ja_[i];
	}

	inline __attribute__((always_inline)) const TValue& operator()(int y, int x) const
	{
		assert(x < dimX);
		assert(y < dimY);

		for (int i = 0; ; )
		{
			for (int row = 0; row < y; row++)
				for (int col = ia_[row]; (col < ia_[row + 1]) && (i < nnZ); col++)
					i++;

			assert (i < nnZ);

			for (int col = ia_[y]; col < ia_[y + 1]; col++, i++)
				if (ja_[i] == x) return a_[i];

			return (TValue) 0;
		}
	}

	inline __attribute__((always_inline)) int dimy() { return dimY; }

	inline __attribute__((always_inline)) int dimx() { return dimX; }

	inline __attribute__((always_inline)) int nnz() { return nnZ; }
		
	inline __attribute__((always_inline)) void resize(int dimY_, int dimX_, int nnz_)
	{
		dimY = dimY_; dimX = dimX_; nnZ = nnz_;
		a_.resize(nnZ);
		ia_.resize(dimY + 1);
		ja_.resize(nnZ);
	}
	
	inline __attribute__((always_inline)) void fill(TValue value)
	{
		std::fill(a_.begin(), a_.end(), value);
	}
};

} // namespace Sparse

} // namespace Host

#if defined(__CUDACC__)

namespace Device {

// Device memory matrix with all rows aligned
template<typename T>
class Dense : public DataContainer<T>
{
	size_t size;
	int dimY, dimX, dimXAligned;

	__host__ __device__
	static int dimAlign(int dim)
	{
		int dimAligned = dim;
		if (dim % AVX_VECTOR_SIZE)
			dimAligned = dim + AVX_VECTOR_SIZE - dim % AVX_VECTOR_SIZE;
		return dimAligned;
	}
	
	__host__ __device__
	static int sizeAlign(int dimY, int dimX)
	{
		return dimY * dimAlign(dimX);
	}

public :
	__host__ __device__
	Dense() : DataContainer<T>(), size(0), dimY(0), dimX(0) { }

	__host__ __device__
	Dense(int dimY_, int dimX_) : DataContainer<T>(sizeAlign(dimY_, dimX_)), size(sizeAlign(dimY_, dimX_)),
		dimY(dimY_), dimX(dimX_), dimXAligned(dimAlign(dimX_)) { }

	__host__ __device__
	~Dense() { }
	
	__host__ __device__
	inline __attribute__((always_inline)) T& operator()(int y, int x)
	{
		assert(x < dimXAligned);
		assert(y < dimY);
		int index = x + dimXAligned * y;
		assert(index < size);
		return DataContainer<T>::modifyDataAt(index);
	}

	__host__ __device__
	inline __attribute__((always_inline)) const T& operator()(int y, int x) const
	{
		assert(x < dimXAligned);
		assert(y < dimY);
		int index = x + dimXAligned * y;
		assert(index < size);
		return const_cast<Dense*>(this)->accessDataAt(index);
	}

	__host__ __device__
	inline __attribute__((always_inline)) int dimy() { return dimY; }

	__host__ __device__
	inline __attribute__((always_inline)) int dimx() { return dimX; }
		
	__host__ __device__
	inline __attribute__((always_inline)) void resize(int dimY_, int dimX_)
	{
		dimY = dimY_; dimX = dimX_;
		dimXAligned = dimAlign(dimX_);
		size = sizeAlign(dimY_, dimX_);
		DataContainer<T>::deinitialize();
		DataContainer<T>::initialize(size);
	}

	template<typename VectorType = std::vector<T, AlignedAllocator::Host<T> > >
	__host__
	void operator=(Matrix::Host::Dense<T, VectorType>& other)
	{
		Matrix::Device::Dense<T>* matrix = this;

		// Use byte container preventing destruction of matrix
		// mirrored from device. Otherwise it will be destroyed
		// together with newly created data array after resizing.
		std::vector<char> container(sizeof(Matrix::Device::Dense<T>));
	
		// Determine, in which memory the current matrix instance
		// resides.
		cudaPointerAttributes attrs;
		CUDA_ERR_CHECK(cudaPointerGetAttributes(&attrs, this));
		if (attrs.memoryType == cudaMemoryTypeDevice)
		{		
			matrix = reinterpret_cast<Matrix::Device::Dense<T>*>(&container[0]);
			CUDA_ERR_CHECK(cudaMemcpy(matrix, this, sizeof(Matrix::Device::Dense<T>),
				cudaMemcpyDeviceToHost));
		}
		
		matrix->resize(other.dimy(), other.dimx());

		// It is assumed safe to copy padded data from host to device matrix,
		// as they use the same memory allocation policy.
		size_t size = (ptrdiff_t)&other(other.dimy() - 1, other.dimx() - 1) -
			(ptrdiff_t)other.getData() + sizeof(T);
		CUDA_ERR_CHECK(cudaMemcpy(matrix->getData(), other.getData(), size,
			cudaMemcpyHostToDevice));

		if (attrs.memoryType == cudaMemoryTypeDevice)
		{		
			CUDA_ERR_CHECK(cudaMemcpy(this, matrix, sizeof(Matrix::Device::Dense<T>),
				cudaMemcpyHostToDevice));
		}
	}
};

namespace Sparse {

// Host memory sparse matrix in CSR format
template<typename TValue, typename TIndex>
class CSR
{
	Vector::Device<TValue> a_;
	Vector::Device<TIndex> ia_, ja_;
	int dimY, dimX, nnZ;

public :
	CSR() : dimY(0), dimX(0), nnZ(0) { }

	CSR(int dimY_, int dimX_, int nnz_) :
		a_(nnz_), ia_(dimY_ + 1), ja_(nnz_),
		dimY(dimY_), dimX(dimX_), nnZ(nnz_) { }

	__host__ __device__
	~CSR() { }

	__host__ __device__
	inline __attribute__((always_inline)) TValue& a(int i) { return a_(i); }

	__host__ __device__
	inline __attribute__((always_inline)) const TValue& a(int i) const { return a_(i); }

	__host__ __device__
	inline __attribute__((always_inline)) TIndex& ia(int i) { return ia_(i); }

	__host__ __device__
	inline __attribute__((always_inline)) const TIndex& IA(int i) const { return ia_(i); }
	
	__host__ __device__
	inline __attribute__((always_inline)) TIndex& ja(int i) { return ja_(i); }

	__host__ __device__
	inline __attribute__((always_inline)) const TIndex& JA(int i) const { return ja_(i); }

	__host__ __device__
	inline __attribute__((always_inline)) const TValue& operator()(int y, int x) const
	{
		assert(x < dimX);
		assert(y < dimY);

		for (int i = 0; ; )
		{
			for (int row = 0; row < y; row++)
				for (int col = ia_[row]; (col < ia_[row + 1]) && (i < nnZ); col++)
					i++;

			assert (i < nnZ);

			for (int col = ia_[y]; col < ia_[y + 1]; col++, i++)
				if (ja_[i] == x) return a_[i];

			return (TValue) 0;
		}
	}

	__host__ __device__
	inline __attribute__((always_inline)) int dimy() { return dimY; }

	__host__ __device__
	inline __attribute__((always_inline)) int dimx() { return dimX; }

	__host__ __device__
	inline __attribute__((always_inline)) int nnz() { return nnZ; }
		
	__host__ __device__
	inline __attribute__((always_inline)) void resize(int dimY_, int dimX_, int nnz_)
	{
		dimY = dimY_; dimX = dimX_; nnZ = nnz_;
		a_.resize(nnZ);
		ia_.resize(dimY + 1);
		ja_.resize(nnZ);
	}

	template<template<typename, typename> class TVector = std::vector, template<typename> class TAllocator = AlignedAllocator::Host>
	__host__
	void operator=(Matrix::Host::Sparse::CSR<TValue, TIndex, TVector, TAllocator>& other)
	{
		Matrix::Device::Sparse::CSR<TValue, TIndex>* matrix = this;

		// Use byte container preventing destruction of matrix
		// mirrored from device. Otherwise it will be destroyed
		// together with newly created data array after resizing.
		std::vector<char> container(sizeof(Matrix::Device::Sparse::CSR<TValue, TIndex>));

		// Determine, in which memory the current matrix instance
		// resides.
		cudaPointerAttributes attrs;
		CUDA_ERR_CHECK(cudaPointerGetAttributes(&attrs, this));
		if (attrs.memoryType == cudaMemoryTypeDevice)
		{		
			matrix = reinterpret_cast<Matrix::Device::Sparse::CSR<TValue, TIndex>*>(&container[0]);
			CUDA_ERR_CHECK(cudaMemcpy(matrix, this,
				sizeof(Matrix::Device::Sparse::CSR<TValue, TIndex>),
				cudaMemcpyDeviceToHost));
		}
		
		matrix->resize(other.dimy(), other.dimx(), other.nnz());

		// It is assumed safe to copy padded data from host to device matrix,
		// as they use the same memory allocation policy.
		{
			size_t size = (ptrdiff_t)&other.a(other.nnz() - 1) -
				(ptrdiff_t)other.a(0) + sizeof(TValue);
			CUDA_ERR_CHECK(cudaMemcpy(matrix->a(0), other.a(0), size,
				cudaMemcpyHostToDevice));
		}
		{
			size_t size = (ptrdiff_t)&other.ia(other.dimY()) -
				(ptrdiff_t)other.ia(0) + sizeof(TIndex);
			CUDA_ERR_CHECK(cudaMemcpy(matrix->ia(0), other.ia(0), size,
				cudaMemcpyHostToDevice));
		}
		{
			size_t size = (ptrdiff_t)&other.ja(other.nnz() - 1) -
				(ptrdiff_t)other.ja(0) + sizeof(TIndex);
			CUDA_ERR_CHECK(cudaMemcpy(matrix->ja(0), other.ja(0), size,
				cudaMemcpyHostToDevice));
		}
		
		if (attrs.memoryType == cudaMemoryTypeDevice)
		{
			CUDA_ERR_CHECK(cudaMemcpy(this, matrix,
				sizeof(Matrix::Device::Sparse::CSR<TValue, TIndex>),
				cudaMemcpyHostToDevice));
		}
	}
};

} // namespace Sparse

} // namespace Device

#else

namespace Device
{
	template<typename T>
	class Dense;
	
	namespace Sparse
	{
		template<typename T>
		class CSR;
	}
}

#endif // __CUDACC__

} // namespace Matrix

} // namespace cuda

#endif // MATRIX_H

