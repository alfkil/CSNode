#include "CSNode.hpp"
#include "Strings.hpp"
#include <sys/socket.h>
#include <netdb.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
bool CSNode::doBind (int port) {
    // server address
    this->port = port;
    struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
    int addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((bindSocket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket");
        return false;
    }

    //set reuseaddr - remember to to this BEFORE bind! ;)
    int j;
    if(setsockopt(bindSocket, SOL_SOCKET, SO_REUSEADDR, &j, sizeof(int)) < 0 ) {
			perror("setsockopt");
            close(bindSocket);
            return false;
		}

    //do binding to INADDR_ANY
    if (bind (bindSocket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close (bindSocket);
        return false;
    }
    hasBinding = true;

    //listen
    if (listen(bindSocket, 10) < 0) {
        close (bindSocket);
        perror("listen");
        return false;
    }
    return true;
}

void CSNode::unBind () {
    if (hasBinding) { close (bindSocket); }
    hasBinding = false;
}

CSNode::CSConnection *CSNode::waitForIncomming(int port) {
    CSConnection *connection = new CSConnection;

    struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;

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

    struct sockaddr_in sa;

	// port = 2121;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_LOOPBACK;

    bool success = true;

    struct hostent *he;

    he = gethostbyname(address);
    if(he == 0) {
        perror("gethostbyname");
        return 0;
    }
    char *addr = (char *)&sa.sin_addr;
    addr[0] = he->h_addr_list[0][0];
    addr[1] = he->h_addr_list[0][1];
    addr[2] = he->h_addr_list[0][2];
    addr[3] = he->h_addr_list[0][3];

    if (connect (connection->connectionSocket, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        success = false;
    }

    if (!success) {
        close (connection->connectionSocket);
        delete connection;
        return 0;
    }

    connection->identityString = address;
    connection->isValid = true;
    return connection;
}

void CSNode::closeConnection (CSNode::CSConnection *connection) {
    if (connection) {
        close (connection->connectionSocket);
    }
}

string CSNode::readSentence (CSConnection *connection, char stopCharacter) { //ETX
    string result;
    const int Bufsize = 1024;
    int bytes; char buffer[1024];

    string str = connection->readBuffer.readString();
    if (str.length())
        return str;

    while ((bytes = recv(connection->connectionSocket, buffer, Bufsize, 0)) >= 0) {
        if (bytes < 0) {
            perror ("recv)");
            return string();
        }
        connection->readBuffer.fill(buffer, bytes);
        if (connection->readBuffer.contains ('\3'))
            break;
    }
    return connection->readBuffer.readString();
}

bool CSNode::writeSentence (CSConnection *connection, string sentence) {
    Buffer writeBuffer;
    writeBuffer.fill ((char *)sentence.c_str(), sentence.length());
    writeBuffer.fill ((char *)"\3\0", 2);
    string out = writeBuffer.readString('\0');
    int bytes = send (connection->connectionSocket, out.c_str(), out.length(), 0);
    if (bytes == sentence.length()) return true;
    return false;
}

int CSNode::clientPUSH (CSNode::CSConnection *connection, const char *filename)
{
    astream fn(filename);
    if(fn.contains('/') || fn.contains(':')) {
        cout << "clientPUSH : Please avoid using paths as descriptors.\n";
        return -1;
    }

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

    int bufSize = 4096;
    char buffer[bufSize];
    int totalRead = 0, totalSent = 0;
    //this below is mirrored in serverPUSH
    try {
    while (totalSent == totalRead && totalRead < size) {
        int bytesRead = read(fd, buffer, bufSize);
        if (bytesRead < 0) { perror ("read"); break; }
        totalRead += bytesRead;
        //first fill the buffer (in case it was already non-empty)
        connection->readBuffer.fill(buffer, bytesRead);
        //...then extract from it
        int bytesAvailable;
        while((bytesAvailable = connection->readBuffer.readBytes(buffer, bufSize)) > 0) {
            for(int i = 0; i < 5; i++) {
                fd_set wfds;
                struct timeval tv;
                tv.tv_sec = 5;
                tv.tv_usec = 0;
                FD_ZERO(&wfds);
                FD_SET(connection->connectionSocket, &wfds);
                int ret = select(connection->connectionSocket+1, 0, &wfds, 0, &tv);
                if(ret < 0) {
                    throw(1);
                }
                if(ret == 0) printf("timeout.\n");
                if(FD_ISSET(connection->connectionSocket, &wfds)) {
                    int bytesSent = 0;
                    while (bytesSent < bytesAvailable) {
                        int bytes = send(connection->connectionSocket, buffer + bytesSent, bytesAvailable - bytesSent, 0);
                        if(bytes < 0) {
                            if(errno == ECONNRESET) {
                                connection->isValid = false;                        
                            }
                            throw(0);
                        }
                        bytesSent += bytes;
                        if(bytesSent < bytesAvailable) printf("Non-complete send.\n");
                    }
                    totalSent += bytesSent;
                    cout << ".";
                    break; //successfull send
                }
            }
        }
    }
    } catch(int kind) {
        if(kind == 1)
            perror("select");
        else perror("send");
    }

    if(totalSent != totalRead)
        cout << "Mismatch between bytes read and bytes sent.\n";
    else
        cout << "ok.\n";

    if (totalSent == size)
        printf("<PUSH> : file sent\n");
    else
        printf("<PUSH> : incomplete send\n");

    close (fd);
    return 0;
}

void CSNode::clientPUSHDIR (CSNode::CSConnection *connection, const char *dirname) {
    astream fn(dirname);
    if(fn.contains('/') || fn.contains(':')) {
        cout << "clientPUSHDIR : Please avoid using paths as descriptors.\n";
        return;
    }

    DIR *dirp;
    struct dirent *dp;

    if ((dirp = opendir(dirname)) == 0) {
        perror("opendir");
        return;
    }

    string cwd = remoteGETCWD(connection);    
    remoteMKDIR(connection, dirname);
    remoteCHDIR(connection, dirname);

    string localcwd = localGETCWD();
    localCHDIR(dirname);

    while ((dp = readdir(dirp)) != 0) {
        DIR *ndirp;
        if(!strcmp(".", dp->d_name) || !strcmp("..", dp->d_name))
            continue;
        if ((ndirp = opendir(dp->d_name)) == 0) {
            string message = "PUSH ";
            message += dp->d_name;
            writeSentence(connection, message);
            clientPUSH(connection, dp->d_name);
        } else {
            closedir(ndirp);
            clientPUSHDIR(connection, dp->d_name);
        }
    }
    closedir(dirp);

    localCHDIR(localcwd);
    remoteCHDIR(connection, cwd.c_str());
}

string CSNode::remoteGETCWD(CSNode::CSConnection *connection) {
    writeSentence(connection, "GETCWD");
    return readSentence(connection);
}
void CSNode::remoteCHDIR(CSNode::CSConnection *connection, const char *dirname) {
    string message = "CHDIR ";
    message += dirname;
    writeSentence(connection, message);
}
void CSNode::remoteMKDIR(CSNode::CSConnection *connection, string dirname) {
    string message = "MKDIR ";
    message += dirname;
    writeSentence(connection, message);
}

string CSNode::localGETCWD() {
    char buffer[PATH_MAX];
    getcwd(buffer, PATH_MAX);
    return string(buffer);
}
void CSNode::localCHDIR(string newdir) {
    int status = chdir(newdir.c_str());
}
CSNode::CSConnection *CSNode::clientCommand(string command, CSNode::CSConnection *connection = 0) {
    astream a(command);
    string stripped = a.get('\n');
    astream b(stripped);
    vector<string> argv = b.split(' ');
    string keyword = argv[0];

    cout << "argv[0]: '" << argv[0] << "'\n";

    if(!keyword.compare("SERVE")) {
        if(argv.size() < 2) {
            cout << "Usage : SERVE <port>\n";
        } else {
            cout << "Server thread started on port " << argv[1] << "....\n";
            server.startThread(atoi(argv[1].c_str()));
        }
    } else if(!keyword.compare("UNSERVE")) {
        cout << "Ending server thread\n";
        server.endThread();
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
        if(connection && connection->isValid) {
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
            if(!connection->isValid) {
                delete connection;
                connection = 0;
            }
        } else cout << "No connection\n";
    } else if (!keyword.compare("PUSHDIR")) {
        if(connection) {
            writeSentence (connection, stripped);
            clientPUSHDIR (connection, argv[1].c_str());
        }
    } else if (!keyword.compare("PULL")) {

    } else if (!keyword.compare("GETCWD")) {
        if(connection) {
            writeSentence (connection, "GETCWD");
            cout << "<GETCWD> : " << readSentence(connection) << "\n";
        }
    }
    return connection;
}

int CSNode::serverPUSH (CSNode::CSConnection *connection, const char *filename) // PUSH from client
{
    astream fn(filename);
    if(fn.contains('/') || fn.contains(':')) {
        cout << "serverPUSH : Please avoid using paths as descriptors.\n";
        return -1;
    }

    //read size from connect
    string sizestr = readSentence(connection);
    int size = atoi(sizestr.c_str());

    if (size == 0) {
    	printf("<PUSH> : file size 0, abort\n");
    	return -1;
    }

    printf("<read file> : %s , size %d\n", filename, size);
    //read file to disk
    int fd = open(filename, O_RDWR|O_CREAT, 0666); //read, write and execute permission
    if (fd < 0) {
	    perror("open");
	    return -1;
    }

    int bufSize = 4096;
    char buffer[bufSize];
    int bytesReceived = 0, bytesWritten = 0;

    while (bytesReceived == bytesWritten && bytesReceived < size && connection->readBuffer.numberOfBytes() > 0) {
		int bytes1 = connection->readBuffer.readBytes(buffer, MIN(size-bytesReceived, bufSize));
        int bytes2 = write(fd, buffer, bytes1);
		bytesReceived += bytes1;
        bytesWritten += bytes2;
//		cout << "bytes(*) : " << bytes1 << " " << bytes2 << " " << bytesWritten << " " << bytesReceived << "\n";
    }
    try {
    while (bytesReceived < size && bytesReceived == bytesWritten) {
        int i;
        for(i = 0; i < 5; i++) {
            if(i > 0) printf(",-");
            fd_set rfds;
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            FD_ZERO(&rfds);
            FD_SET(connection->connectionSocket, &rfds);
            int ret = select(connection->connectionSocket+1, &rfds, 0, 0, &tv);
            if(ret == 0) printf("select returned zero.\n");
            if(ret < 0) {
                throw(1);
            }
            if(FD_ISSET(connection->connectionSocket, &rfds)) {
                int bytes = 0;
                while (bytes == 0)
                    bytes = recv(connection->connectionSocket, buffer, MIN(bufSize, size-bytesReceived), 0);
                if (bytes < 0) {
                    if(errno == ECONNRESET) {
                        connection->isValid = false;
                    }
                    throw(0);
                }
                bytesReceived += bytes;
                //first fill the buffer (in case it was already non-empty)
                connection->readBuffer.fill(buffer, bytes);
                //...then extract from it
                int bytes1, bytes2;
                while((bytes1 = connection->readBuffer.readBytes(buffer, MIN(bufSize, size - bytesWritten))) > 0) {
                    bytes2 = write(fd, buffer, bytes1);
                    bytesWritten += bytes2;
                }
                break;
            }
        }
        if(i == 5) throw(2);
        cout << ".";
    }
    cout << "ok.\n";
    } catch(int kind) {
        if(kind == 1) perror("select");
        else if(kind == 0) perror("recv");
        else printf("Trials max reached. Abandonning recv.\n");
    }

    if(bytesWritten == size)
        printf("<PUSH> : success\n");
    else
        printf("<PUSH> : Odd file size\n");

    close(fd);
    return 0;
}

void CSNode::serverCommand (CSConnection *connection) {
    bool end = false;
    while(!end) {
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
            end = true;
            // exit(0); // abandon...
        } else if (!keyword.compare("PUSH")) {
            serverPUSH(connection, argv[1].c_str());
            if(!connection->isValid) {
                delete connection;
                connection = 0;
            }
            end = true;
        } else if (!keyword.compare("PULL")) {

        } else if (!keyword.compare("CHDIR")) {
            if(argv.size() < 2) {
                cout << "Usage : CHDIR <dirname>\n";
            } else {
                cout << "<CHDIR> : " << argv[1] << "\n";
                int status = chdir(argv[1].c_str());
                if(status == 0) {
                    // cout << "Success (CHDIR)\n";
                }
            }
        } else if (!keyword.compare("GETCWD")) {
            cout << "<GETCWD>\n";
            char buffer[PATH_MAX];
            getcwd(buffer, PATH_MAX);
            writeSentence(connection, string(buffer));
        } else if (!keyword.compare("MKDIR")) {
            if(argv.size() < 2) {
                cout << "Usage : MKDIR <dirname>\n";
            } else {
                cout << "<MKDIR> : " << argv[1] <<  "\n";
                int status = mkdir(argv[1].c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
                if(status == 0) {
                    // cout << "Success (MKDIR)\n";
                }
            }
        }
    }
    printf("> "); //reinsert the prompt
}
