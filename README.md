[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/0ek2UV58)

# DOC++
A scalable, concurrent, TCP-based Networked File Storage System built using Name Server – Storage Server – Client architecture.
Supports concurrent reads and writes, sentence-level locking, access control, metadata management, streaming, file execution, undo system, caching, and more.

This project demonstrates strong skills in systems programming, concurrency, networking, sockets, data structures, file systems, and OS concepts.

---

## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Core Features](#core-features)
- [User Operations](#user-operations)
- [System Requirements](#system-requirements)
- [Technical Specifications](#technical-specifications)
- [Bonus Features](#bonus-features)
- [Implementation Details](#implementation-details)
- [Getting Started](#getting-started)

---

## Overview

This Network File System is a distributed storage solution that enables multiple users to concurrently access, read, and write files across multiple storage servers. The system maintains data persistence, enforces access control, and provides efficient file operations through a centralized Name Server architecture.

### Key Highlights

- **Concurrent Access**: Multiple users can simultaneously interact with files, with sentence-level locking for write operations
- **Distributed Storage**: Files are stored across multiple Storage Servers with seamless access
- **Access Control**: Fine-grained permissions with read/write access management
- **Real-time Streaming**: Word-by-word content streaming with simulated delay
- **Data Persistence**: All files and metadata persist across server restarts
- **Efficient Search**: Sub-linear time complexity for file lookups using optimized data structures

---

## System Architecture

The system consists of three primary components:

### 1. Name Server (NM)
- Central coordinator managing all client-server communication
- Maintains file-to-storage mappings and user access control
- Handles file metadata and routing decisions
- Implements efficient search with caching mechanisms

### 2. Storage Servers (SS)
- Physically store and retrieve file data
- Support dynamic addition of new servers during runtime
- Execute commands issued by the Name Server
- Handle direct client connections for streaming operations

### 3. User Clients
- Interface for all file operations
- Support username-based authentication
- Communicate with Name Server for routing
- Establish direct connections to Storage Servers when needed

---

## Core Features

### File Management

- **Text-Only Files**: All files contain text data organized into sentences and words
- **Sentence Structure**: Sentences delimited by `.`, `!`, or `?`
- **No Size Limits**: System handles both small and large documents efficiently
- **Concurrent Editing**: Multiple users can edit different sentences simultaneously

### Access Control

- **Owner Permissions**: File creators have full read/write access
- **Granular Access**: Separate read and write permissions for other users
- **Access Management**: Owners can add or remove user permissions dynamically

### Data Integrity

- **Undo Functionality**: Revert last changes made to any file
- **Sentence Locking**: Prevents simultaneous edits to the same sentence
- **Persistent Storage**: All data survives server restarts

---

## User Operations

### File Viewing & Listing

```bash
VIEW           # Lists all files user has access to
VIEW -a        # Lists all files on the system
VIEW -l        # Lists user-access files with details (word count, character count, etc.)
VIEW -al       # Lists all system files with details
```

### File Operations

```bash
CREATE <filename>                    # Creates an empty file
READ <filename>                      # Displays complete file content
INFO <filename>                      # Shows file metadata (size, permissions, timestamps)
DELETE <filename>                    # Removes file from system (owner only)
```

### Writing to Files

```bash
WRITE <filename> <sentence_number>   # Locks sentence for editing
<word_index> <content>               # Updates word at specified index
...
ETIRW                                # Releases lock and commits changes
```

### Advanced Operations

```bash
STREAM <filename>                    # Streams content word-by-word (0.1s delay)
EXEC <filename>                      # Executes file content as shell commands
UNDO <filename>                      # Reverts last change to file
```

### User & Access Management

```bash
LIST                                 # Lists all registered users
ADDACCESS -R <filename> <username>   # Grants read access
ADDACCESS -W <filename> <username>   # Grants write (and read) access
REMACCESS <filename> <username>      # Removes all access
```

---

## System Requirements

### Data Persistence
All files and metadata (access control lists, file attributes) are stored persistently and survive server restarts.

### Access Control Enforcement
The system validates user permissions before allowing any file operation, ensuring only authorized users can access files.

### Comprehensive Logging
- All requests, responses, and acknowledgments are logged with timestamps
- Includes IP addresses, ports, usernames, and operation details
- Facilitates debugging and system monitoring

### Error Handling
Universal error codes cover scenarios including:
- Unauthorized access attempts
- File not found errors
- Resource contention (locked files)
- System failures and network issues

### Efficient Search
- Sub-O(N) time complexity for file lookups
- Implemented using Tries, Hashmaps, or similar structures
- Caching mechanism for recent searches

---

## Technical Specifications

### Initialization Sequence

1. **Name Server**: Starts first with publicly known IP and port
2. **Storage Servers**: Register with NM, providing:
   - IP address and ports (NM connection & client connection)
   - List of stored files
3. **Clients**: Connect with username, IP, and port information

### Client-Server Interaction Flow

#### Read/Write/Stream Operations
- NM identifies appropriate SS and returns connection details
- Client establishes direct connection with SS
- Data transfer continues until STOP packet or completion

#### Metadata Operations (LIST, INFO, ACCESS)
- NM handles requests directly from local storage
- Returns information without involving SS

#### Create/Delete Operations
- NM forwards request to appropriate SS
- SS executes and sends ACK to NM
- NM relays confirmation to client

#### Execute Operations
- NM retrieves file content from SS
- NM executes commands and captures output
- Output streamed back to client

---

## Bonus Features

### 1. Hierarchical Folder Structure 

Organize files in folders and subfolders for better management.

```bash
CREATEFOLDER <foldername>            # Creates new folder
MOVE <filename> <foldername>         # Moves file to folder
VIEWFOLDER <foldername>              # Lists folder contents
```

### 2. Checkpointing System 

Save and restore file states at specific points in time.

```bash
CHECKPOINT <filename> <tag>          # Creates checkpoint with tag
VIEWCHECKPOINT <filename> <tag>      # Views checkpoint content
REVERT <filename> <tag>              # Reverts to checkpoint state
LISTCHECKPOINTS <filename>           # Lists all checkpoints
```

### 3. Access Request System 

Users can request access to files they don't own.

- Request storage mechanism for pending access requests
- Owner-side interface to view and approve/reject requests
- No push notifications required
  ```bash
  REQUESTACCESS <file> <-R/-W>               # user can request access for other files
  VIEWREQUESTS [file]                        # user can view pending requests for the files owned by them
  APPROVEREQUEST <file> <username> <-R/-W>   # user can approve pending requests
  DENYREQUEST <file> <username>              # user can deny pending requests
  ```

### 4. Fault Tolerance & Replication 

Robust data replication strategy for high availability.

**Replication Strategy**:
- Every file duplicated across multiple Storage Servers
- Asynchronous write propagation to replicas
- NM retrieves from replicas on primary SS failure

**Failure Detection**:
- NM monitors SS availability
- Automatic failover to replica stores

**Recovery Mechanism**:
- Reconnected SS synchronized with replica state
- Seamless data consistency restoration

### 5. Innovation Showcase 
- custom ```help``` function that lists the commands along with their synatx.

---

## Implementation Details

### Technologies Used

- **Protocol**: TCP sockets for reliable communication
- **Standards**: POSIX C library compliance
- **Debugging**: Wireshark for packet inspection
- **Testing**: Netcat for component stub testing

### Design Principles

- **Modular Architecture**: Clean separation of concerns across components
- **Efficient Data Structures**: Tries/Hashmaps for O(1) or O(log N) lookups
- **Concurrency Handling**: Sentence-level locking with swap file mechanism
- **Error Resilience**: Comprehensive error codes throughout system

### Assumptions

- Name Server failure results in complete system shutdown (by design)
- All file content is ASCII text
- Public knowledge of Name Server IP and port

---

## Getting Started

### Prerequisites

- POSIX-compliant C compiler (gcc recommended)
- Linux/Unix environment
- TCP socket support

### Building the Project

```bash
# Clone the repository
git clone <repository-url>
cd course-project-siscalls

# Compile components
make 

# Or compile individually
make nameserver
make storageserver
make client
```

### Running the System

```bash
# Terminal 1: Start Name Server
./name_server <nm_ip> <nm_port>

# Terminal 2+: Start Storage Servers
./storage_server <ss_ip> <ss_port_for_clients> <nm_ip> <nm_port>

# Terminal N: Start Clients
./client <nm_port> <nm_ip>
```

### Testing

```bash
# Run test suite
make test

# Or use netcat for component testing
nc localhost <port>
```

---

## Project Structure

```
course-project-siscalls/
├── github/
├── include/
│   ├── cache.h
│   ├── client_handler.h
│   ├── client.h
│   ├── common.h
│   ├── executor.h
│   ├── init.h
│   ├── logger.h
│   ├── name_server.h
│   ├── persistence.h
│   ├── protocol.h
│   ├── search.h
│   ├── socket_utils.h
│   ├── storage_manager.h
│   ├── storage_server.h
│   └── user_manager.h
├── src/
│   ├── client/
│   │   └── main.c
│   ├── common/
│   │   ├── error_handler.c
│   │   ├── logger.c
│   │   ├── protocol.c
│   │   └── socket_utils.c
│   ├── name_server/
│   │   ├── cache.c
│   │   ├── client_handler.c
│   │   ├── executor.c
│   │   ├── init.c
│   │   ├── main.c
│   │   ├── search.c
│   │   ├── storage_manager.c
│   │   └── user_manager.c
│   └── storage_server/
│       ├── init.c
│       ├── main.c
│       └── persistence.c
├── test/
├── Makefile
├── README.md
└── server.log
```

---

## Future Enhancements

- Multi-threaded Storage Server for improved concurrency
- Encryption for secure data transmission
- Web-based client interface
- Distributed Name Server for fault tolerance

---

## Author

[Mahek Desai]  
[mahek.desai@students.iiit.ac.in]  
[LinkedIn/GitHub Profile]


[Laveena Jain]  
[laveena.jain@research.iiit.ac.in]  
[LinkedIn/GitHub Profile]

---

## Acknowledgments

This project implements a distributed file system with concurrent access control, demonstrating proficiency in network programming, distributed systems, and system design.

**Resources**: All implementations are original with documented assumptions where applicable.


   
