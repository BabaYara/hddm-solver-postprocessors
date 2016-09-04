#include "check.h"
#include "Data.h"
#include "interpolator.h"

#include <fstream>
#include <iostream>
#include <mpi.h>

using namespace cuda;
using namespace std;

int Data::getNno() const { return nno; }

class Data::Host::DataHost
{
	std::vector<Matrix<int>::Host> index;
	std::vector<Matrix<real>::Host> surplus, surplus_t;

public :

	DataHost(int nstates)
	{
		index.resize(nstates);
		surplus.resize(nstates);
		surplus_t.resize(nstates);
	}

	friend class Data::Host;
};

void Data::load(const char* filename, int istate)
{
	MPI_Process* process;
	MPI_ERR_CHECK(MPI_Process_get(&process));
	const Parameters& params = Interpolator::getInstance()->getParameters();

	if (loadedStates[istate])
	{
		cerr << "State " << istate << " data is already loaded" << endl;
		process->abort();
	}

	ifstream infile;
	if (params.binaryio)
		infile.open(filename, ios::in | ios::binary);
	else
		infile.open(filename, ios::in);

	if (!infile.is_open())
	{
		cerr << "Error opening file: " << filename << endl;
		process->abort();
	}

	infile >> dim;
	if (dim != params.nagents)
	{
		cerr << "File \"" << filename << "\" # of dimensions (" << dim << 
			") mismatches config (" << params.nagents << ")" << endl;
		process->abort();
	}
	infile >> nno; 
	infile >> TotalDof;
	infile >> Level;

	// Pad all indexes to 4-element boundary.
	vdim = dim / AVX_VECTOR_SIZE;
	if (dim % AVX_VECTOR_SIZE) vdim++;
	int nsd = 2 * vdim * AVX_VECTOR_SIZE;

	Matrix<int>::Host& index = *host.getIndex(istate);
	Matrix<real>::Host& surplus = *host.getSurplus(istate);
	Matrix<real>::Host& surplus_t = *host.getSurplus_t(istate);
	
	index.resize(nno, nsd);
	index.fill(0);
	surplus.resize(nno, TotalDof);
	surplus.fill(0.0);

	// For better caching we use transposed surplus.
	surplus_t.resize(TotalDof, nno);
	surplus_t.fill(0.0);
	int j = 0;
	while (infile)
	{
		if (j == nno) break;

		for (int i = 0; i < dim; )
		{
			for (int v = 0; (v < AVX_VECTOR_SIZE) && (i < dim); v++, i++)
			{
				int value; infile >> value;
				value = 2 << (value - 2);
				index(j, i) = value;
			}
		}
		for (int i = 0; i < dim; )
		{
			for (int v = 0; (v < AVX_VECTOR_SIZE) && (i < dim); v++, i++)
			{
				int value; infile >> value;
				value--;
				// Percompute "j" to merge two cases into one:
				// (((i) == 0) ? (1) : (1 - fabs((x) * (i) - (j)))).
				if (!index(j, i)) value = 0;
				index(j, i + vdim * AVX_VECTOR_SIZE) = value;
			}
		}
		for (int i = 0; i < TotalDof; i++)
		{
			double value; infile >> value;
			surplus(j, i) = value;
			surplus_t(i, j) = value;
		}
		j++;
	}
	infile.close();
	
	loadedStates[istate] = true;

	// Copy data from host to device memory.
	device.setIndex(istate, &index);
	device.setSurplus(istate, &surplus);
	device.setSurplus_t(istate, &surplus_t);
}

void Data::clear()
{
	fill(loadedStates.begin(), loadedStates.end(), false);
}

Data::Data(int nstates_) : nstates(nstates_), host(nstates), device(nstates)
{
	loadedStates.resize(nstates);
	fill(loadedStates.begin(), loadedStates.end(), false);
}

Data::Host::Host(int nstates)
{
	data.reset(new DataHost(nstates));
}

Matrix<int>::Host* Data::Host::getIndex(int istate)
{
	return &data->index[istate];
}

Matrix<real>::Host* Data::Host::getSurplus(int istate)
{
	return &data->surplus[istate];
}

Matrix<real>::Host* Data::Host::getSurplus_t(int istate)
{
	return &data->surplus_t[istate];
}

// Determine the size of type on device.
template<typename T>
static __global__ void deviceSizeOf(size_t* result)
{
	*result = sizeof(T);
}

template<typename T>
static __global__ void constructDeviceData(int length, char* ptr)
{
	for (int i = 0; i < length; i++)
		new(&ptr[i * sizeof(T)]) T();
}

template<typename T>
static __global__ void destroyDeviceData(int length, char* ptr)
{
	for (int i = 0; i < length; i++)
		delete (T*)&ptr[i * sizeof(T)];
}

// Host array of elements in device memory, whose size is
// determined on device in runtime.
template<typename T>
class DeviceSizedArray
{
	int length;
	size_t size;
	char* ptr;

public :

	DeviceSizedArray(int length_) : length(length_)
	{
		// Determine the size of target type.
		size_t* dSize;
		CUDA_ERR_CHECK(cudaMalloc(&dSize, sizeof(size_t)));
		deviceSizeOf<T><<<1, 1>>>(dSize);
		CUDA_ERR_CHECK(cudaGetLastError());
		CUDA_ERR_CHECK(cudaDeviceSynchronize());
		CUDA_ERR_CHECK(cudaMemcpy(&size, dSize, sizeof(size_t), cudaMemcpyDeviceToHost));
		CUDA_ERR_CHECK(cudaFree(dSize));
		
		// Make sure the size of type is the same on host and on device.
		if (size != sizeof(T))
		{
			cerr << "Unexpected unequal sizes of type T in DeviceSizedArray<T> on host and device" << endl;
			MPI_Process* process;
			MPI_ERR_CHECK(MPI_Process_get(&process));
			process->abort();
		}

		// Allocate array.		
		CUDA_ERR_CHECK(cudaMalloc(&ptr, size * length));
		
		// Construct individual array elements from within the device kernel code,
		// using placement new operator.
		constructDeviceData<T><<<1, 1>>>(length, ptr);
		CUDA_ERR_CHECK(cudaGetLastError());
		CUDA_ERR_CHECK(cudaDeviceSynchronize());
	}
	
	~DeviceSizedArray()
	{
		// Destroy individual array elements from within the device kernel code.
		destroyDeviceData<T><<<1, 1>>>(length, ptr);
		CUDA_ERR_CHECK(cudaGetLastError());
		CUDA_ERR_CHECK(cudaDeviceSynchronize());			

		// Delete each indivudual element on host, which triggers deletion of
		// data buffer previously allocated on host with cudaMalloc.
		vector<T> elements;
		elements.resize(length);
		CUDA_ERR_CHECK(cudaMemcpy(&elements[0], ptr, length * sizeof(T), cudaMemcpyDeviceToHost));
		for (int i = 0; i < length; i++)
		{
			// Set that matrix owns its underlying data buffer.
			elements[i].disownData();

			delete &elements[i];
		}

		// Free device memory used for array elements.
		CUDA_ERR_CHECK(cudaFree(ptr));
	}

	T& operator[](int i)
	{
		return (T&)ptr[i * size];
	}
};

class Data::Device::DataDevice
{
	// These containers shall be entirely in device memory, including vectors.
	DeviceSizedArray<Matrix<int>::Device> index;
	DeviceSizedArray<Matrix<real>::Device> surplus, surplus_t;

public :

	DataDevice(int nstates) : index(nstates), surplus(nstates), surplus_t(nstates) { }

	friend class Data::Device;
};	

Data::Device::Device(int nstates_) : nstates(nstates_)
{
	data.reset(new DataDevice(nstates_));
}

Matrix<int>::Device* Data::Device::getIndex(int istate)
{
	return &data->index[istate];
}

Matrix<real>::Device* Data::Device::getSurplus(int istate)
{
	return &data->surplus[istate];
}

Matrix<real>::Device* Data::Device::getSurplus_t(int istate)
{
	return &data->surplus_t[istate];
}

template<typename T>
static void setMatrix(int istate,
	MatrixHost<T, std::vector<T, AlignedAllocator<T> > > * pMatrixHost, MatrixDevice<T>* pMatrixDevice)
{
	MatrixHost<T, std::vector<T, AlignedAllocator<T> > >& matrixHost = *pMatrixHost;

	MatrixDevice<T> matrixDevice;
	CUDA_ERR_CHECK(cudaMemcpy(&matrixDevice, pMatrixDevice, sizeof(MatrixDevice<T>), cudaMemcpyDeviceToHost));
	matrixDevice.resize(matrixHost.dimy(), matrixHost.dimx());

	// It is assumed to be safe to copy padded data from host to device matrix,
	// as they use the same memory allocation policy.
	size_t size = (ptrdiff_t)&matrixHost(matrixHost.dimy() - 1, matrixHost.dimx() - 1) -
		(ptrdiff_t)matrixHost.getData() + sizeof(T);
	CUDA_ERR_CHECK(cudaMemcpy(matrixDevice.getData(), pMatrixHost->getData(), size, cudaMemcpyHostToDevice));

	// Set that matrix does not own its underlying data buffer.
	matrixDevice.disownData();

	CUDA_ERR_CHECK(cudaMemcpy(pMatrixDevice, &matrixDevice, sizeof(MatrixDevice<T>), cudaMemcpyHostToDevice));
}

void Data::Device::setIndex(int istate, Matrix<int>::Host* matrix)
{
	setMatrix<int>(istate, matrix, &data->index[istate]);
}

void Data::Device::setSurplus(int istate, Matrix<real>::Host* matrix)
{
	setMatrix<double>(istate, matrix, &data->surplus[istate]);
}

void Data::Device::setSurplus_t(int istate, Matrix<real>::Host* matrix)
{
	setMatrix<double>(istate, matrix, &data->surplus_t[istate]);
}
		
extern "C" Data* getData(int nstates)
{
	return new Data(nstates);
}

