#include "CSNode.hpp"
#include "Strings.hpp"
bool CSNode::doBind (int port) {
    //if (hasBinding) return true;

    // server address
    this->port = port;
    struct sockaddr_in address = (struct sockaddr_in) {
        AF_INET,
        htons((sa_family_t)port),
        (in_port_t){INADDR_ANY}
    };
    int addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((bindSocket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket");
        return false;
    }

    //do binding to INADDR_ANY
    if (bind (bindSocket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close (bindSocket);
        return false;
    }

    //set reuseaddr
    int j;
    if(setsockopt(bindSocket, SOL_SOCKET, SO_REUSEADDR, &j, sizeof(int)) < 0 ) {
			perror("setsockopt");
            close(bindSocket);
            return false;
		}

    //listen
    if (listen(bindSocket, 10) < 0) {
        close (bindSocket);
        perror("listen");
        return false;
    }
    hasBinding = true;
    return true;
}

void CSNode::unBind () {
    if (hasBinding) close (bindSocket);
    hasBinding = false;
}

CSNode::CSConnection *CSNode::waitForIncomming(int port) {
    if(!hasBinding) {
        doBind(port);
    }
    CSConnection *connection = new CSConnection;

    struct sockaddr_in address = (struct sockaddr_in) {
        AF_INET,
        htons((sa_family_t)port),
        (in_port_t){INADDR_ANY}
    };
    int addrlen = sizeof(address);
    
    if ((connection->connectionSocket = accept(bindSocket, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
        perror("accept");
        return 0;
    }

    //convert incomming address to presentation text
    char addressBuffer[1024];
    if (inet_ntop (AF_INET, &address.sin_addr, addressBuffer, sizeof(addressBuffer)) > 0) {
        connection->identityString = addressBuffer;
    }

    return connection;
}

CSNode::CSConnection *CSNode::connectToPeer (const char *address, int port) {
    CSConnection *connection = new CSConnection;
    if ((connection->connectionSocket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
    	return 0;
    }

    struct sockaddr_in saddr;
    memset(&saddr, '0', sizeof(saddr));

    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);

    bool success = true;
    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, address, &saddr.sin_addr) <= 0) {
	    printf("<inet_pton> : Invalid address /or Address not supported \n");
        success = false;
    }

    if (connect (connection->connectionSocket, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect");
        success = false;
    }

    if (!success) {
        close (connection->connectionSocket);
        delete connection;
        return 0;
    }

    connection->identityString = address;
    return connection;
}

void CSNode::closeConnection (CSNode::CSConnection *connection) {
    if (connection) {
        close (connection->connectionSocket);
        delete connection;
    }
}

string CSNode::readSentence (CSConnection *connection, char stopCharacter) { //ETX
    string result;
    const int Bufsize = 1024;
    int bytes; char buffer[1024];

    string test = connection->readBuffer.read();
    if (test.length())
        return test;

    while ((bytes = recv(connection->connectionSocket, buffer, Bufsize, 0)) >= 0) {
        if (bytes < 0) {
            perror ("recv)");
            exit (0);
        }
        connection->readBuffer.fill(buffer, bytes);
        if (connection->readBuffer.contains ('\3'))
            break;
    }
    return connection->readBuffer.read();
}

bool CSNode::writeSentence (CSConnection *connection, string sentence) {
    Buffer writeBuffer;
    writeBuffer.fill ((char *)sentence.c_str(), sentence.length());
    writeBuffer.fill ((char *)"\3\0", 2);
    string out = writeBuffer.read('\0');
    int bytes = send (connection->connectionSocket, out.c_str(), out.length(), 0);
    if (bytes == sentence.length()) return true;
    return false;
}

int CSNode::clientPUSH (CSNode::CSConnection *connection, const char *filename)
{
    int fd = open (filename,  O_RDONLY);
    if (fd < 0) {
        writeSentence(connection, "0");
	    perror ("open");
	    return -1;
    }

    //calculate file size
    int size = lseek (fd, 0, SEEK_END);
    lseek (fd, 0, SEEK_SET);
    char sizebuf[128];

    //send file size as string
    sprintf(sizebuf, "%d", size);
    writeSentence(connection, sizebuf);

    printf("<send file> : %s , size=%s\n", filename, sizebuf);

    int i;
    for (i = 0; i < size; i++) {
        //construct string with single byte
        char byte;
        int len = read(fd, &byte, 1);
        if(len != 1) break;
        // len = send(connection->connectionSocket, &byte, 1, 0);
        // if(len != 1) break;
        char bytebuf[32];
        sprintf(bytebuf, "%d", byte);
        writeSentence(connection, bytebuf);
    }
    if (i == size)
        printf("<PUSH> : file sent\n");
    else
        printf("<PUSH> : incomplete send\n");

    close (fd);
    return 0;
}

CSNode::CSConnection *CSNode::clientCommand(string command, CSNode::CSConnection *connection = 0) {
    astream a(command);
    string stripped = a.get('\n');
    a.setString(stripped);
    vector<string> argv = a.split(' ');
    string keyword = argv[0];

    cout << "argv[0]: '" << argv[0] << "'\n";

    if(!keyword.compare("SERVE")) {
        if(argv.size() < 2) {
            cout << "Usage : SERVE <port>\n";
        } else {
            cout << "Server thread started on port " << argv[1] << "....\n";

            doBind(atoi(argv[1].c_str()));
            server.startThread();
            // CSNode::CSConnection *newConnection = waitForIncomming (atoi(argv[1].c_str()));
            // if(newConnection) {
            //     cout << "Gracefully accepted call from " << newConnection->identityString << " :)\n";
            //     connection = newConnection;
            //     serverCommand (connection);
            // }
        }
    } else if(!keyword.compare("CALL")) {
        if(argv.size() < 3) {
            cout << "Usage : CALL <address> <port>\n";
        } else {
            if (connection) {
                cout << "Please close previous connection to " << connection->identityString << " first. (CALL)\n";
                return connection;
            }

            CSNode::CSConnection *newConnection = connectToPeer(argv[1].c_str(), atoi(argv[2].c_str()));

            if (newConnection) {
                cout << "Successfully connected to " << argv[1] << "\n";
                cout << "Sending credentials to " << argv[1] << "\n";
                // -- bla bla --
                cout << "Host accepted call.\n";
                connection = newConnection;
                //node.createServer (connection);
            } else cout << "Failed to connect to " << argv[1] << "\n";
        }
        return connection;
    } else if (!keyword.compare("MESSAGE")) {
        if(connection) {
            writeSentence (connection, stripped);
        } else cout << "No connection\n";
    } else if (!keyword.compare("CLOSE")) {
        if(connection) {
            writeSentence(connection, "CLOSE");
            closeConnection(connection);
            cout << "Connection closed\n";
            connection = 0;
        } else cout << "Not connected.\n";
    } else if (!keyword.compare("EXIT")) {
        if (connection) {
            writeSentence (connection, "CLOSE");
            closeConnection (connection);
        }
        cout << "Connection closed. Exit.\n";
        exit(0);
    } else if (!keyword.compare("PUSH")) {
        if(connection) {
            writeSentence (connection, stripped);
            clientPUSH (connection, argv[1].c_str());
        } else cout << "No connection\n";
    } else if (!keyword.compare("PULL")) {

    }
    return connection;
}

int CSNode::serverPUSH (CSNode::CSConnection *connection, const char *filename) // PUSH from client
{
    //read size from connect
    string sizestr = readSentence(connection);
    int size = atoi(sizestr.c_str());

    if (size == 0) {
    	printf("<PUSH> : file size 0, abort\n");
    	return -1;
    }

    printf("<read file> : %s , size %d\n", filename, size);
    //read file to disk
    int fd = open (filename, O_CREAT|O_WRONLY|O_TRUNC); //read, write and execute permission
    if (fd < 0) {
	    perror("open");
	    return -1;
    }

    int i;
    for (i = 0; i < size; i++) {
        string input = readSentence(connection);
        char number = atoi(input.c_str());
        // char number;
        // int len = recv(connection->connectionSocket, &number, 1, 0);
        // if(len != 1) break;
        int len = write (fd, &number, 1);
        if(len != 1) break;
    }
    if(i == size)
        printf("<PUSH> : success\n");
    else
        printf("<PUSH> : Odd file size\n");

    return 0;
}

void CSNode::serverCommand (CSConnection *connection) {
    string message = readSentence(connection);
    astream a(message);
    vector<string> argv = a.split(' ');
    string keyword = argv[0];

    if (!keyword.compare("MESSAGE")) {
        string output = "<message> : ";
        for (int i = 1; i < argv.size(); i++) {
            output += argv[i];
            if (i < argv.size() - 1) output += " ";
        }
        cout << output  << "\n";
    } else if (!keyword.compare("CLOSE")) {
        // if remote node is a server, help to close
        writeSentence(connection, "CLOSE");
        closeConnection (connection);
        exit(0); // abandon...
    } else if (!keyword.compare("PUSH")) {
        serverPUSH(connection, argv[1].c_str());
    } else if (!keyword.compare("PULL")) {

    }
    printf("> "); //reinsert the prompt
}