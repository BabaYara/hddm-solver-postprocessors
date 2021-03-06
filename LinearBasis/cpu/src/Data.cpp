#include "check.h"
#include "Data.h"
#include "interpolator.h"

#include <fstream>
#include <iostream>
#include <mpi.h>

using namespace cpu;
using namespace std;

int Data::getNno() const { return nno; }

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

struct IndexPair
{
	unsigned short i, j;
};

template<typename T>
static void read_index(ifstream& infile, int nno, int dim, int vdim, Matrix<int>& index_)
{
	MPI_Process* process;
	MPI_ERR_CHECK(MPI_Process_get(&process));

	char index_marker[] = "index";
	infile.read(index_marker, strlen(index_marker));

	unsigned int index_nonzeros = 0;
	infile.read(reinterpret_cast<char*>(&index_nonzeros), sizeof(unsigned int));

	vector<int> A;
	A.resize(index_nonzeros);
	for (int i = 0, e = A.size(); i != e; i++)
		infile.read(reinterpret_cast<char*>(&A[i]), sizeof(IndexPair));

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
	JA.resize(index_nonzeros);
	for (int i = 0, e = JA.size(); i != e; i++)
	{
		T value;
		infile.read(reinterpret_cast<char*>(&value), sizeof(T));
		JA[i] = (int)value;
	}

	for (int i = 0, e = JA.size(); i != e; i++)
	{
		if (JA[i] >= dim)
		{
			cerr << "JA[i] must be within column index range - not true for JA[" << i << "] = " << JA[i] << endl;
			process->abort();
		}
	}

	for (int i = 0, row = 0; row < nno; row++)
		for (int col = IA[row]; col < IA[row + 1]; col++, i++)
		{
			IndexPair& pair = (IndexPair&)A[i];
			index_(row, JA[i]) = pair.i;
			index_(row, JA[i] + vdim * AVX_VECTOR_SIZE) = pair.j;
		}
	
	//cout << (100 - (double)index_nonzeros / (nno * dim) * 100) << "% index sparsity" << endl;
}

template<typename T>
static void read_surplus(ifstream& infile, int nno, int TotalDof, Matrix<double>& surplus_)
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
			surplus_(row, JA[i]) = A[i];
	
	//cout << (100 - (double)surplus_nonzeros / (nno * TotalDof) * 100) << "% surplus sparsity" << endl;
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
	int nsd = 2 * vdim * AVX_VECTOR_SIZE;

	index[istate].resize(nno, nsd);
	index[istate].fill(0);
	surplus[istate].resize(nno, TotalDof);
	surplus[istate].fill(0.0);

	// For better caching we use transposed surplus.
	surplus_t[istate].resize(TotalDof, nno);
	surplus_t[istate].fill(0.0);
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
					index[istate](j, i) = value;
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
					if (!index[istate](j, i)) value = 0;
					index[istate](j, i + vdim * AVX_VECTOR_SIZE) = value;
				}
			}
			for (int i = 0; i < TotalDof; i++)
			{
				double value; infile >> value;
				surplus[istate](j, i) = value;
				surplus_t[istate](i, j) = value;
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
			read_index<unsigned char>(infile, nno, dim, vdim, index[istate]);
			break;
		case 2 :
			read_index<unsigned short>(infile, nno, dim, vdim, index[istate]);
			break;
		case 4 :
			read_index<unsigned int>(infile, nno, dim, vdim, index[istate]);
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
			read_surplus<unsigned char>(infile, nno, TotalDof, surplus[istate]);
			break;
		case 2 :
			read_surplus<unsigned short>(infile, nno, TotalDof, surplus[istate]);
			break;
		case 4 :
			read_surplus<unsigned int>(infile, nno, TotalDof, surplus[istate]);
			break;
		}
	}
	
	infile.close();
	
	loadedStates[istate] = true;
}

void Data::clear()
{
	fill(loadedStates.begin(), loadedStates.end(), false);
}

Data::Data(int nstates_) : nstates(nstates_)
{
	index.resize(nstates);
	surplus.resize(nstates);
	surplus_t.resize(nstates);
	loadedStates.resize(nstates);
	fill(loadedStates.begin(), loadedStates.end(), false);
}

extern "C" Data* getData(int nstates)
{
	return new Data(nstates);
}

