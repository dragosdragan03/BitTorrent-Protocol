# BitTorrent Protocol Implementation

## Overview

This project implements a simplified BitTorrent protocol for peer-to-peer file sharing using MPI (Message Passing Interface). In this implementation, clients can be seeds (initially have complete files) or peers (acquire files during execution). To reduce tracker load, it only stores file hashes and keeps track of which clients have which files, either partially or completely.

## Protocol Description

BitTorrent is a decentralized peer-to-peer file sharing protocol. Unlike traditional client-server models, BitTorrent allows users to distribute data in a decentralized manner:

1. Files are split into segments with cryptographic hashes for integrity verification
2. Clients can download segments from multiple sources simultaneously
3. As clients acquire segments, they become sources for other clients
4. The workload of distributing files is shared among all participants

### Client Roles

In the BitTorrent protocol, a client can have one of three roles for a particular file:

- **Seed**: Owns the complete file and can share it with others (only uploads)
- **Peer**: Owns some segments of a file and is interested in acquiring the rest (both uploads and downloads)
- **Leecher**: Either owns no segments of a desired file or doesn't share the segments it has (only downloads)

A client can be a seed for one file and a peer for another simultaneously.

### Tracker Role

The tracker assists in communication between clients by maintaining lists of who has what files (but not which segments). It coordinates file transmission without actually participating in the file sharing process.

## Implementation Details

### Project Structure

The project consists of multiple MPI processes:
- Rank 0: Acts as the tracker
- Ranks 1+: Act as clients (seeds and peers)

Each client has two threads:
- **Download Thread**: Handles file downloading and tracker communication
- **Upload Thread**: Responds to segment requests from other clients

### Data Structures

Each client maintains:
- `hashes`: Maps files to their segment hashes
- `own_files`: Files the client initially has and reports to the tracker as a seed
- `desire_files`: Files the client wants to download
- `desired_hashes`: Hashes of segments that have been downloaded during execution

The tracker maintains:
- `owners_files`: Maps files to clients that own them (seeds and peers)
- `all_hashes`: Maps files to their segment hashes

### Helper Functions

- `send_message`: Sends a string message
- `receive_message`: Receives a string message

### Client Operation Flow

1. **Initialization**
   - Read input file
   - Report owned files to the tracker
   - Wait for confirmation from tracker to begin downloads

2. **Download Process**
   - For each desired file:
     - Request segment hashes and seed list from tracker
     - For each segment not owned:
       - Find a seed/peer with the segment
       - Request segment from that seed/peer
       - Mark segment as received
     - Periodically (every 10 segments) update the seed list from tracker
     - After receiving segments, update tracker to include client in seed list
     - Save downloaded file

3. **Upload Process**
   - Continuously listen for segment requests
   - When a request is received:
     - Check if segment exists in `hashes` (initially owned) or `desired_hashes` (downloaded)
     - If exists, send segment to requester
     - If not, send "DOESN'T_EXIST" message

4. **Completion**
   - Inform tracker when all files are downloaded
   - Close download thread but keep upload thread running
   - When all clients finish, close upload thread and terminate

### Tracker Operation Flow

1. **Initialization**
   - Wait for initial messages from all clients
   - Record which clients have which files
   - Send acknowledgment to clients to begin file sharing

2. **Message Handling**
   - Process different message types:
     - `START_DOWNLOAD`: Provide hashes for a requested file
     - `SEED_LIST`: Provide list of seeds/peers for a file
     - `UPDATE`: Update list of clients that have a file
     - `FINISH`: Mark a client as completed
   - When all clients finish, send termination message to all clients

### Race Condition Prevention

A mutex is used in download and upload thread functions to prevent race conditions when accessing or modifying the `desired_hashes` data structure.

### Efficiency Optimization

Clients are selected in rotation for downloads to avoid overwhelming a single client. Each client downloads one segment at a time, then moves to another client for the next segment.

## Building and Running

### Prerequisites
- MPI library
- C++ compiler with C++11 support
- pthread library

### Compilation
```
make
```

### Execution
```
make run
```
or manually:
```
mpirun -np <NUM_PROCESSES> ./bittorrent
```
where `<NUM_PROCESSES>` is at least 3 (1 tracker + minimum 2 clients)

### Cleaning
```
make clean
```

## Input Format

Each client reads from a file named `in<RANK>.txt` with the following format:
```
<number_of_owned_files>
<file_name_1> <number_of_segments_1>
<hash_of_segment_1>
<hash_of_segment_2>
...
<file_name_2> <number_of_segments_2>
...
<number_of_desired_files>
<desired_file_name_1>
<desired_file_name_2>
...
```

## Output Format

For each downloaded file, the client creates an output file named `client<RANK>_<FILENAME>` containing the hashes of all segments in order.

## Author

Dragan Dragos Ovidiu, 333CAb, 2024-2025