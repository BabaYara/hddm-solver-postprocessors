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
	Vector::Host<Matrix::Host::Sparse::CSR<IndexPair, uint32_t> > index;
	Vector::Host<Matrix::Host::Dense<real> > surplus, surplus_t;

public :

	DataHost(int nstates)
	{
		index.resize(nstates);
		surplus.resize(nstates);
		surplus_t.resize(nstates);
	}

	friend class Data::Host;
};

static bool isCompressed(const char* filename)
{
	MPI_Process* process;
	MPI_ERR_CHECK(MPI_Process_get(&process));

	ifstream infile;
	infile.open(filename, ifstream::binary);

	if (!infile.is_open())
	{
		cerr << "Error opening file: " << filename << endl;
		process->abort();
	}

	bool compressed = false;
	
	char format_marker[] = "          \0";
		
	infile.read(format_marker, strlen(format_marker));
	
	infile.close();
	
	if ((string)format_marker == "compressed")
		compressed = true;
	
	return compressed;
}

template<typename T>
static void read_index(ifstream& infile, int nno, int nsd, int dim, Matrix::Host::Sparse::CSR<IndexPair, uint32_t>& index)
{
	MPI_Process* process;
	MPI_ERR_CHECK(MPI_Process_get(&process));

	char index_marker[] = "index";
	infile.read(index_marker, strlen(index_marker));

	unsigned int index_nonzeros = 0;
	infile.read(reinterpret_cast<char*>(&index_nonzeros), sizeof(unsigned int));
	
	index.resize(nno, nsd, index_nonzeros);

	for (int i = 0, e = index_nonzeros; i < e; i++)
		infile.read(reinterpret_cast<char*>(&index.a(i)), sizeof(IndexPair));

	infile.read(reinterpret_cast<char*>(&index.ia(0)), sizeof(T));
	for (int i = 1, e = nno + 1; i < e; i++)
	{
		T value;
		infile.read(reinterpret_cast<char*>(&value), sizeof(T));
		index.ia(i) = (uint32_t)value + index.ia(i - 1);
	}
	
	if (index.ia(0) != 0)
	{
		cerr << "IA[0] must be 0" << endl;
		process->abort();
	}

	for (int i = 1, e = nno + 1; i < e; i++)
	{
		if (index.ia(i) < index.ia(i - 1))
		{
			cerr << "IA[i] must be not less than IA[i - 1] - not true for IA[" << i << "] >= IA[" << (i - 1) <<
				"] : " << index.ia(i) << " < " << index.ia(i - 1) << endl;
			process->abort();
		}
	}
	
	for (int i = 0; i < index_nonzeros; i++)
		infile.read(reinterpret_cast<char*>(&index.ja(i)), sizeof(T));
	
	for (int i = 0; i < index_nonzeros; i++)
	{
		if (index.ja(i) >= dim)
		{
			cerr << "JA[i] must be within column index range - not true for JA[" << i << "] = " << index.ja(i) << endl;
			process->abort();
		}
	}

	/*int good_rows = 0;
	for (int j = 0; j < nno; j++)
	{
		int inc = 1;
		for (int i = index.ia(j); i < index.ia(j + 1); i++)
		{
			const IndexPair& pair = index.a(i);
			if (pair.i < pair.j + 1)
			{
				cout << "nno " << j << ": i = " << pair.i << ", j = " << pair.j << " do not hold i >= j + 1" << endl;
				inc = 0;
			}
		}
		good_rows += inc;
	}
	cout << "Overall, " << good_rows << " good nno rows (" << (good_rows / (double)nno * 100) << "%)" << endl;*/
}

template<typename T>
static void read_surplus(ifstream& infile, int nno, int TotalDof,
	Matrix::Host::Dense<double>& surplus, Matrix::Host::Dense<double>& surplus_t)
{
	MPI_Process* process;
	MPI_ERR_CHECK(MPI_Process_get(&process));

	char surplus_marker[] = "surplus";
	infile.read(surplus_marker, strlen(surplus_marker));

	unsigned int surplus_nonzeros = 0;
	infile.read(reinterpret_cast<char*>(&surplus_nonzeros), sizeof(unsigned int));

	vector<double> A;
	A.resize(surplus_nonzeros);
	for (int i = 0, e = A.size(); i != e; i++)
		infile.read(reinterpret_cast<char*>(&A[i]), sizeof(double));

	vector<int> IA;
	IA.resize(nno + 1);
	{
		T value;
		infile.read(reinterpret_cast<char*>(&value), sizeof(T));
		IA[0] = value;
	}
	for (int i = 1, e = IA.size(); i != e; i++)
	{
		T value;
		infile.read(reinterpret_cast<char*>(&value), sizeof(T));
		IA[i] = (int)value + IA[i - 1];
	}

	if (IA[0] != 0)
	{
		cerr << "IA[0] must be 0" << endl;
		process->abort();
	}

	for (int i = 1, e = IA.size(); i != e; i++)
		if (IA[i] < IA[i - 1])
		{
			cerr << "IA[i] must be not less than IA[i - 1] - not true for IA[" << i << "] >= IA[" << (i - 1) <<
				"] : " << IA[i] << " < " << IA[i - 1] << endl;
			process->abort();
		}
	
	vector<int> JA;
	JA.resize(surplus_nonzeros);
	for (int i = 0, e = JA.size(); i != e; i++)
	{
		T value;
		infile.read(reinterpret_cast<char*>(&value), sizeof(T));
		JA[i] = (int)value;
	}

	for (int i = 0, e = JA.size(); i != e; i++)
	{
		if (JA[i] >= TotalDof)
		{
			cerr << "JA[i] must be within column index range - not true for JA[" << i << "] = " << JA[i] << endl;
			process->abort();
		}
	}

	for (int i = 0, row = 0; row < nno; row++)
		for (int col = IA[row]; col < IA[row + 1]; col++, i++)
		{
			surplus(row, JA[i]) = A[i];
			surplus_t(JA[i], row) = A[i];
		}
}

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

	bool compressed = isCompressed(filename);

	ifstream infile;
	if (compressed)
		infile.open(filename, ifstream::binary);
	else
		infile.open(filename, ios::in);

	if (!infile.is_open())
	{
		cerr << "Error opening file: " << filename << endl;
		process->abort();
	}

	if (compressed)
	{
		char format_marker[] = "          ";		
		infile.read(format_marker, strlen(format_marker));
		infile.read(reinterpret_cast<char*>(&dim), sizeof(int));
		infile.read(reinterpret_cast<char*>(&nno), sizeof(int)); 
		infile.read(reinterpret_cast<char*>(&TotalDof), sizeof(int));
		infile.read(reinterpret_cast<char*>(&Level), sizeof(int));
	}
	else
	{	
		infile >> dim;
		infile >> nno; 
		infile >> TotalDof;
		infile >> Level;
	}

	if (dim != params.nagents)
	{
		cerr << "File \"" << filename << "\" # of dimensions (" << dim << 
			") mismatches config (" << params.nagents << ")" << endl;
		process->abort();
	}

	// Pad all indexes to 4-element boundary.
	vdim = dim / AVX_VECTOR_SIZE;
	if (dim % AVX_VECTOR_SIZE) vdim++;
	int nsd = vdim * AVX_VECTOR_SIZE;

	Matrix::Host::Sparse::CSR<IndexPair, uint32_t>& index = *host.getIndex(istate);
	Matrix::Host::Dense<real>& surplus = *host.getSurplus(istate);
	Matrix::Host::Dense<real>& surplus_t = *host.getSurplus_t(istate);
	
	surplus.resize(nno, TotalDof);
	surplus.fill(0.0);

	// For better caching we use transposed surplus.
	surplus_t.resize(TotalDof, nno);
	surplus_t.fill(0.0);

	if (!compressed)
	{	
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
					IndexPair& pair = (IndexPair&)index(j, i);
					pair.i = value;
				}
			}
			for (int i = 0; i < dim; )
			{
				for (int v = 0; (v < AVX_VECTOR_SIZE) && (i < dim); v++, i++)
				{
					int value; infile >> value;
					value--;
					// Precompute "j" to merge two cases into one:
					// (((i) == 0) ? (1) : (1 - fabs((x) * (i) - (j)))).
					IndexPair& pair = (IndexPair&)index(j, i);
					if (!pair.i) value = 0;
					pair.j = value;
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
	}
	else
	{
		int szt = 0;
		if (dim <= numeric_limits<unsigned char>::max())
			szt = 1;
		else if (dim <= numeric_limits<unsigned short>::max())
			szt = 2;
		else
			szt = 4;
	
		//cout << "Using " << szt << "-byte IA/JA size for index" << endl;
	
		switch (szt)
		{
		case 1 :
			read_index<unsigned char>(infile, nno, nsd, dim, index);
			break;
		case 2 :
			read_index<unsigned short>(infile, nno, nsd, dim, index);
			break;
		case 4 :
			read_index<unsigned int>(infile, nno, nsd, dim, index);
			break;
		}

		if (TotalDof <= numeric_limits<unsigned char>::max())
			szt = 1;
		else if (TotalDof <= numeric_limits<unsigned short>::max())
			szt = 2;
		else
			szt = 4;
	
		//cout << endl;
		//cout << "Using " << szt << "-byte IA/JA size for surplus" << endl;
	
		switch (szt)
		{
		case 1 :
			read_surplus<unsigned char>(infile, nno, TotalDof, surplus, surplus_t);
			break;
		case 2 :
			read_surplus<unsigned short>(infile, nno, TotalDof, surplus, surplus_t);
			break;
		case 4 :
			read_surplus<unsigned int>(infile, nno, TotalDof, surplus, surplus_t);
			break;
		}
	}

	infile.close();
	
	loadedStates[istate] = true;

	// Copy data from host to device memory.
	device.setIndex(istate, index);
	device.setSurplus(istate, surplus);
	device.setSurplus_t(istate, surplus_t);
}

void Data::clear()
{
	fill(loadedStates.begin(), loadedStates.end(), false);
}

const Data::Host* Data::getHost() const
{
	return &host;
}

const Data::Device* Data::getDevice() const
{
	return &device;
}

Data::Data(int nstates_) : nstates(nstates_), host(nstates), device(nstates)
{
	loadedStates.resize(nstates);
	fill(loadedStates.begin(), loadedStates.end(), false);
}

const Data::Host::DataHost* Data::Host::getHost() const
{
	return data.get();
}

Data::Host::DataHost* Data::Host::getHost()
{
	return data.get();
}

Data::Host::Host(int nstates)
{
	data.reset(new DataHost(nstates));
}

const Matrix::Host::Sparse::CSR<IndexPair, uint32_t>* Data::Host::getIndex(int istate) const
{
	return &data->index(istate);
}

const Matrix::Host::Dense<real>* Data::Host::getSurplus(int istate) const
{
	return &data->surplus(istate);
}

const Matrix::Host::Dense<real>* Data::Host::getSurplus_t(int istate) const
{
	return &data->surplus_t(istate);
}

Matrix::Host::Sparse::CSR<IndexPair, uint32_t>* Data::Host::getIndex(int istate)
{
	return &data->index(istate);
}

Matrix::Host::Dense<real>* Data::Host::getSurplus(int istate)
{
	return &data->surplus(istate);
}

Matrix::Host::Dense<real>* Data::Host::getSurplus_t(int istate)
{
	return &data->surplus_t(istate);
}

class Data::Device::DataDevice
{
	// These containers shall be entirely in device memory, including vectors.
	Vector::Device<Matrix::Device::Sparse::CRW<IndexPair, uint32_t> > index;
	Vector::Device<Matrix::Device::Dense<real> > surplus, surplus_t;

public :

	DataDevice(int nstates) : index(nstates), surplus(nstates), surplus_t(nstates) { }

	friend class Data::Device;
};	

const Data::Device::DataDevice* Data::Device::getData() const
{
	return data.get();
}

Data::Device::DataDevice* Data::Device::getData()
{
	return data.get();
}

Data::Device::Device(int nstates_) : nstates(nstates_)
{
	data.reset(new DataDevice(nstates_));
}

const Matrix::Device::Sparse::CRW<IndexPair, uint32_t>* Data::Device::getIndex(int istate) const
{
	return &data->index(istate);
}

const Matrix::Device::Dense<real>* Data::Device::getSurplus(int istate) const
{
	return &data->surplus(istate);
}

const Matrix::Device::Dense<real>* Data::Device::getSurplus_t(int istate) const
{
	return &getData()->surplus_t(istate);
}

Matrix::Device::Sparse::CRW<IndexPair, uint32_t>* Data::Device::getIndex(int istate)
{
	return &data->index(istate);
}

Matrix::Device::Dense<real>* Data::Device::getSurplus(int istate)
{
	return &data->surplus(istate);
}

Matrix::Device::Dense<real>* Data::Device::getSurplus_t(int istate)
{
	return &data->surplus_t(istate);
}

void Data::Device::setIndex(int istate, Matrix::Host::Sparse::CSR<IndexPair, uint32_t>& matrix)
{
	data->index(istate).setReadOnly(false);
	data->index(istate) = matrix;
	data->index(istate).setReadOnly(true);
}

void Data::Device::setSurplus(int istate, Matrix::Host::Dense<real>& matrix)
{
	data->surplus(istate).setReadOnly(false);
	data->surplus(istate) = matrix;
	data->surplus(istate).setReadOnly(true);
}

void Data::Device::setSurplus_t(int istate, Matrix::Host::Dense<real>& matrix)
{
	data->surplus_t(istate).setReadOnly(false);
	data->surplus_t(istate) = matrix;
	data->surplus_t(istate).setReadOnly(true);
}
		
extern "C" Data* getData(int nstates)
{
	return new Data(nstates);
}

