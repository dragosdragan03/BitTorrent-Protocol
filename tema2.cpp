#include <iostream>     
#include <fstream>       
#include <string>        
#include <unordered_map> 
#include <vector>        
#include <filesystem>    
#include <mpi.h>         
#include <pthread.h> 
#include <algorithm>

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_SIZE_MSG 100
#define PATH "in"

struct ClientsArgs {
	int number_of_files;
	unordered_map<string, vector<string>> hashes; // hashes from each file of the client -> filename : vector<hashes>
	vector<string> own_files; // what files contains each client
	vector<string> desired_files; // what files need each client
	unordered_map<string, vector<string>> desired_hashes; // the hashes received from owners/peers
};

struct TrackerArgs {
	// the owners of the files (owners/peers both in the same array) files -> client1, client2...
	unordered_map<string, vector<int>> owners_files;
	// all hahses from all files; file -> hash1, hash2, hash
	unordered_map<string, vector<string>> all_hashes;
};

pthread_mutex_t mutex;
ClientsArgs clientArgs;

/* function used to send a string (meesage) */
void send_message(const string &message, int dest_rank, int tag) {

	string buffer = message.substr(0, MAX_SIZE_MSG - 1);  // Truncate if too long

	MPI_Send(buffer.c_str(), MAX_SIZE_MSG, MPI_CHAR, dest_rank, tag, MPI_COMM_WORLD);
}

/* function used to receive a string (message)*/
string receive_message(MPI_Status &status, int source, int tag) {
	char buffer[MAX_SIZE_MSG];

	if (source == -1) {
		MPI_Recv(buffer, MAX_SIZE_MSG, MPI_CHAR, MPI_ANY_SOURCE, tag, MPI_COMM_WORLD, &status);
	} else {
		MPI_Recv(buffer, MAX_SIZE_MSG, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
	}

	return string(buffer);
}

// 1. Request the file size from tracker to determine how many segments to request.
// 2. Receive a list of owner files(owners / peers) and the names of the hashes that need to be downloaded from clients from the tracker.
// 3. Request the necessary segments from owners / peers based on the hash names and record them in desired_hashes.

void *download_thread_func(void *arg)
{
	MPI_Status status;
	int rank = *(int *)arg;
	string message;

	while (!clientArgs.desired_files.empty()) { // i make a loop util i finish all desired files to download
		vector<string> hashes;
		vector<int> owners;
		bool first_time_seed = false;
		string file_name = clientArgs.desired_files.back();
		clientArgs.desired_files.pop_back();

		// i send a message to tracker to start the download of a file
		send_message("START_DOWNLOAD", TRACKER_RANK, 1);

		// i have to send the file i want the size
		send_message(file_name, TRACKER_RANK, 1);

		// i have to receive the lenght of the file (number of hashes)
		int number_hashes;
		MPI_Recv(&number_hashes, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, &status);
		for (int i = 0; i < number_hashes; i++) {
			string hash = receive_message(status, TRACKER_RANK, 1);

			hashes.push_back(hash);
		}

		// i have to receive from the clients the segments
		int number_owners;
		int current_segment = 0; // this is the counter for the segment
		// Choose a file owner one at a time to diversify client interactions 
		// and avoid overloading any single owner for an extended period.
		int current_client = 0;
		while (current_segment < number_hashes) {

			if (current_segment % 10 == 0) { // i have to retrieve the newest list of owners from the tracker
				// i have to receive the vector with owners/peers
				owners.clear();
				send_message("SEED_LIST", TRACKER_RANK, 1);

				send_message(file_name, TRACKER_RANK, 1); // i have to send the filename to know the owners 

				MPI_Recv(&number_owners, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE); // number of owners
				for (int i = 0; i < number_owners; i++) {
					int seed_number;
					MPI_Recv(&seed_number, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE); // number of owners
					owners.push_back(seed_number);
				}

				// i have to update the tracker with the current client wich is downloading the file
				if (!first_time_seed && current_segment != 0) {
					send_message("UPDATE", TRACKER_RANK, 1);

					send_message(file_name, TRACKER_RANK, 1);

					MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
					first_time_seed = true;
				}
			}

			if (owners[current_client] != rank) { // i have to send a request to a client 
				send_message(file_name, owners[current_client], 2); // first i have to send the file_name
				send_message(hashes[current_segment], owners[current_client], 2); // i send the hash to the owner of the file
			} else {
				current_client = (current_client + 1) % number_owners;
				continue;
			}

			string response = receive_message(status, owners[current_client], 3);
			if (response != "DOESN'T_EXIST") { // i have to verify if the owner has the requested hash
				pthread_mutex_lock(&mutex);
				clientArgs.desired_hashes[file_name].push_back(response);
				pthread_mutex_unlock(&mutex);
			} else { // this means the client doesn't have the requested hash
				current_segment--;
			}

			current_client = (current_client + 1) % number_owners;
			current_segment++;
		}

		string file_to_open = "client" + to_string(rank) + "_" + file_name;
		ofstream out(file_to_open);
		int len = clientArgs.desired_hashes[file_name].size();
		for (int i = 0; i < len; i++) {
			if (i == len - 1) {
				out << clientArgs.desired_hashes[file_name][i];
			} else {
				out << clientArgs.desired_hashes[file_name][i] << endl;
			}
		}
		out.close();
	}
	// i send a FINISH message to tracker to announce it that the current client finished to download all his files
	send_message("FINISH", TRACKER_RANK, 1);

	return NULL;
}

void *upload_thread_func(void *arg)
{
	MPI_Status status;

	while (true) {

		// first i have to receive a message to verify if is from another client or from the tracker
		string file_name = receive_message(status, -1, 2);
		if (file_name == "CLOSE_THREAD") { // this means all clients finished to download the files
			break;
		}

		string hash = receive_message(status, status.MPI_SOURCE, 2);

		if (clientArgs.hashes.find(file_name) != clientArgs.hashes.end()) { // i have to verify if the client has the hash - from the begginening (SEED)
			if (find(clientArgs.hashes[file_name].begin(), clientArgs.hashes[file_name].end(), hash) != clientArgs.hashes[file_name].end()) { // this means the hash exist
				send_message(hash, status.MPI_SOURCE, 3);
				continue;
			}
		}
		// it's also necessary to verify if the client has received the specific hash during the process (it wasn't available from the start) (PEER).
		pthread_mutex_lock(&mutex);
		if (clientArgs.desired_hashes.find(file_name) != clientArgs.desired_hashes.end()) {
			if (find(clientArgs.desired_hashes[file_name].begin(), clientArgs.desired_hashes[file_name].end(), hash) !=
				clientArgs.desired_hashes[file_name].end()) {
				send_message(hash, status.MPI_SOURCE, 3);
				pthread_mutex_unlock(&mutex);
				continue;
			}
		}
		pthread_mutex_unlock(&mutex);
		// this means the client doesnt have the req hash
		send_message("DOESN'T_EXIST", status.MPI_SOURCE, 3);
	}

	return NULL;
}

/* function to receive the data from clients to tracker */
void receive_structure(int source_rank, TrackerArgs &trackerArgs) {
	MPI_Status status;
	int size;

	// first i have to receive the hashes
	MPI_Recv(&size, 1, MPI_INT, source_rank, 0, MPI_COMM_WORLD, &status); // number of files
	for (int i = 0; i < size; i++) {

		string filename = receive_message(status, source_rank, 0);

		int file_size;
		MPI_Recv(&file_size, 1, MPI_INT, source_rank, 0, MPI_COMM_WORLD, &status);

		for (int j = 0; j < file_size; j++) {
			string hash = receive_message(status, source_rank, 0);
			if (trackerArgs.all_hashes.find(filename) != trackerArgs.all_hashes.end()) {
				if (find(trackerArgs.all_hashes[filename].begin(), trackerArgs.all_hashes[filename].end(), hash) !=
					trackerArgs.all_hashes[filename].end()) { // this means the hash already exist exist
					continue;
				}
			}
			trackerArgs.all_hashes[filename].push_back(hash);
		}
	}

	// i have to recieve the own files from the clients
	MPI_Recv(&size, 1, MPI_INT, source_rank, 0, MPI_COMM_WORLD, &status);
	for (int i = 0; i < size; i++) {
		string filename = receive_message(status, source_rank, 0);

		trackerArgs.owners_files[filename].push_back(source_rank);
	}

}

void tracker(int numtasks, int rank) {
	MPI_Status status;
	int finished = 0;

	TrackerArgs trackerArgs;
	for (int i = 1; i < numtasks; i++) { // i have to wait for all clients to read the files
		receive_structure(i, trackerArgs);
	}

	// Send "ACK" to all clients except the root
	for (int i = 1; i < numtasks; i++) {
		send_message("ACK", i, 0);
	}

	while (true) {

		if (finished == numtasks - 1) { // if all finished to download all the files
			// i have to send a message to all clients to notify them that to close the upload
			for (int i = 1; i < numtasks; i++) {
				send_message("CLOSE_THREAD", i, 2);
			}
			break;
		}

		string message = receive_message(status, -1, 1);

		if (message == "START_DOWNLOAD") {
			string filename = receive_message(status, status.MPI_SOURCE, 1);

			// i send the number of the hashes
			int number_hashes = trackerArgs.all_hashes[filename].size();
			MPI_Send(&number_hashes, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD);

			// i have to send the hashes to the client to know what to retrieve from the owners/peers
			for (const auto &iter : trackerArgs.all_hashes[filename]) {
				send_message(iter, status.MPI_SOURCE, 1);
			}

		} else if (message == "SEED_LIST") {
			string filename = receive_message(status, status.MPI_SOURCE, 1);

			int number_seeds = trackerArgs.owners_files[filename].size();
			MPI_Send(&number_seeds, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
			for (const auto &iter : trackerArgs.owners_files[filename]) {
				MPI_Send(&iter, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
			}

		} else if (message == "UPDATE") {
			string filename = receive_message(status, status.MPI_SOURCE, 1);

			int new_rank;
			MPI_Recv(&new_rank, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, &status);

			trackerArgs.owners_files[filename].push_back(new_rank);
		} else if (message == "FINISH") {
			finished++;
		}
	}
}

void read_file(int rank) {

	string file_to_open = PATH + to_string(rank) + ".txt";
	ifstream f(file_to_open);

	if (!filesystem::exists(file_to_open)) {
		cout << "Fisierul " << file_to_open << " nu exista" << endl;
		f.close();
	}

	f >> clientArgs.number_of_files;

	for (int i = 0; i < clientArgs.number_of_files; i++) {
		string file_name;
		f >> file_name;
		clientArgs.own_files.push_back(file_name);

		int nr_hashes;
		f >> nr_hashes;

		for (int j = 0; j < nr_hashes; j++) {
			string word;
			f >> word;
			clientArgs.hashes[file_name].push_back(word);
		}
	}

	int nr_files;
	f >> nr_files;

	for (int i = 0; i < nr_files; i++) {
		string file_name;
		f >> file_name;
		clientArgs.desired_files.push_back(file_name);
	}

	f.close();
}

void send_structure() {

	//send the hashes
	int size = clientArgs.hashes.size();
	MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD); // number of files

	for (const auto &iter : clientArgs.hashes) {
		send_message(iter.first, TRACKER_RANK, 0);
		int file_size = iter.second.size();
		MPI_Send(&file_size, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD); // number of hashes/ file size

		for (const auto &pair : iter.second) { // hasehs of the files
			send_message(pair, TRACKER_RANK, 0);
		}
	}

	// send own files
	size = clientArgs.own_files.size();
	MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

	for (int i = 0; i < size; i++) {
		send_message(clientArgs.own_files[i], TRACKER_RANK, 0);
	}
}

void peer(int numtasks, int rank) {
	pthread_t download_thread;
	pthread_t upload_thread;
	void *status;
	MPI_Status status_received;
	int r;

	// i have to retain the files
	read_file(rank);

	send_structure();

	string message = receive_message(status_received, TRACKER_RANK, 0);

	// the client must wait for all other clients to complete reading the files before proceeding
	if (message != "ACK") {
		cout << "Nu a primit confirmare";
		return;
	}

	r = pthread_create(&download_thread, NULL, download_thread_func, (void *)&rank);
	if (r) {
		printf("Eroare la crearea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&rank);
	if (r) {
		printf("Eroare la crearea thread-ului de upload\n");
		exit(-1);
	}

	r = pthread_join(download_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_join(upload_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de upload\n");
		exit(-1);
	}
}

int main(int argc, char *argv[]) {
	int numtasks, rank;

	int provided;
	pthread_mutex_init(&mutex, NULL);
	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	if (provided < MPI_THREAD_MULTIPLE) {
		fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
		exit(-1);
	}
	MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	if (rank == TRACKER_RANK) {
		tracker(numtasks, rank);
	} else {
		peer(numtasks, rank);
	}

	MPI_Finalize();
	pthread_mutex_destroy(&mutex);
}
