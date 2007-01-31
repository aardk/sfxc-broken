/* Author(s): Nico Kruithof, 2007
 * 
 * $Id$
 */

#include <types.h>
#include <Manager_node.h>
#include <Input_node.h>
#include <Output_node.h>
#include <Correlator_node.h>

//global variables
#include <runPrms.h>
#include <genPrms.h>
#include <staPrms.h>
#include <constPrms.h>
//declaration and default settings run parameters
RunP RunPrms;
//declaration and default settings general parameters
GenP GenPrms;
//station parameters class, declaration and default settings
StaP StaPrms[NstationsMax];
// used for randomising numbers for Headers in Mk4 file
UINT32 seed;


#include <iostream> 
#include <assert.h>


int main(int argc, char *argv[]) {
  // MPI
  int rank;

  //initialisation
  int status = MPI_Init(&argc,&argv);
  if (status != MPI_SUCCESS) {
    std::cout << "Error starting MPI program. Terminating.\n";
    MPI_Abort(MPI_COMM_WORLD, status);
    return 1;
  }

  // get the ID (rank) of the task, fist rank=0, second rank=1 etc.
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  ///////////////////////////
  //  The real work
  ///////////////////////////
  if (rank == 0) {
    // get the number of tasks set at commandline (= number of processors)
    int numtasks;
    MPI_Comm_size(MPI_COMM_WORLD,&numtasks);
    
    Manager_node manager(numtasks, rank, argv[1]);
    manager.start();
  } else {
    MPI_Status status, status2;
    MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    switch (status.MPI_TAG) {
    case MPI_TAG_SET_INPUT_NODE_FILE: 
      {
        assert(status.MPI_SOURCE == 0);

        Input_node input_node(rank);
        input_node.start();
        break;
      }
    case MPI_TAG_SET_OUTPUT_NODE_FILE: 
      {
        assert(rank==RANK_OUTPUT_NODE);
        assert(status.MPI_SOURCE == 0);

        Output_node output_node(rank);
        output_node.start();
        break;
      }
    case MPI_TAG_SET_LOG_NODE_COUT: 
    case MPI_TAG_SET_LOG_NODE_FILE: 
      {
        int numtasks;
        MPI_Comm_size(MPI_COMM_WORLD,&numtasks);

        assert(rank==RANK_LOG_NODE);
        assert(status.MPI_SOURCE == 0);

        Log_node log_node(rank, numtasks);
        log_node.start();
        break;
      }
    case MPI_TAG_SET_CORRELATOR_NODE: 
      {
        int job;
        MPI_Recv(&job, 1, MPI_INT32, 0, 
                 MPI_ANY_TAG, MPI_COMM_WORLD, &status2);
        assert(status.MPI_SOURCE == status2.MPI_SOURCE);
        assert(status.MPI_TAG == status2.MPI_TAG);

        Correlator_node correlator(rank);
        correlator.start();
      break;
    }
    default:
      std::cout << "Unknown node type" << std::endl;
      assert(false);
      return 1;
    }
  }

  //close the mpi stuff
  MPI_Barrier( MPI_COMM_WORLD );
  MPI_Finalize();

  return 0;
}
