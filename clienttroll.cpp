/*

Parts of this are taken from fromtroll.c and totroll.c, which are written by:
 * Mitchell Tasman
 * December 1987
 * Modified by Marvin Solomon, October 1989.
and some parts are taken from getaddrinfo(7) man page.

Rest belongs to Doruk Bildibay, e2237089.
*/
#include <sys/syscall.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>

#include <queue>
#include <string>
#include <fstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace std;

const string outFilename("clientOut.txt");

void senderAckOnly(int s);
void senderSR(int s);
void receiverMain(int s);


typedef struct Packet {
	char isAck;
	int seqNumber;
	int checksum;
	char contents[8];
} Packet;

// readonly globals?
struct sockaddr_in trolladdr;

// Global vars that need sync
queue<Packet*> dataPacketSendQueue;
mutex dataQueueMutex;
std::condition_variable cvDataNotEmpty;

queue<Packet*> ackPacketSendQueue;
mutex ackQueueMutex;
std::condition_variable cvAckNotEmpty;


// since this is both sender & receiver, we have two sets of variables for Selective Repeat.
long sendBaseSeqNum = 0;
long sendNextSeqNum = 0;

#define SEND_WINDOW_SIZE 15
#define RCV_WINDOW_SIZE 15

int senderSrPacketAckStates[SEND_WINDOW_SIZE];

int main(int argc, char *argv[])
{
	int sock;
	Packet message;
	struct hostent *host;
	struct sockaddr_in localaddr;
	fd_set selectmask;
	int counter, n;

	// 0 is execname, 1 is IP , 2 is troll port, 3 is local port.
	if ((host = gethostbyname(argv[1])) == NULL) {
		fprintf(stderr, "%s: Unknown troll host '%s'\n",argv[0],argv[1]);
		exit(1);
	}  

	u_short port = atoi(argv[2]);
	if (port < 1024 || port > 0xffff) {
		fprintf(stderr, "%s: Bad troll port %d (must be between 1024 and %d)\n",
			argv[0], port, 0xffff);
		exit(1);
	}

	bzero ((char *)&trolladdr, sizeof trolladdr);
	trolladdr.sin_family = AF_INET;
	bcopy(host->h_addr, (char*)&trolladdr.sin_addr, host->h_length);
	trolladdr.sin_port = htons(port);

	/* get local port ... */

	port = atoi(argv[3]);
	if (port < 1024 || port > 0xffff) {
		fprintf(stderr, "%s: Bad local port %d (must be between 1024 and %d)\n",
			argv[0], port, 0xffff);
		exit(1);
	}

	/* create a socket for sending... */

	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("totroll socket");
		exit(1);
	}
	FD_ZERO(&selectmask);
	FD_SET(sock, &selectmask);

	/* ... and bind its local address and the port*/
	bzero((char *)&localaddr, sizeof localaddr);
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = INADDR_ANY; /* let the kernel fill this in */
	localaddr.sin_port = htons(port);
	if (bind(sock, (struct sockaddr *)&localaddr, sizeof localaddr) < 0) {
		perror("client bind");
		exit(1);
	}

	int optval = 1;
	setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof(int));

	for(int i = 0 ; i < SEND_WINDOW_SIZE; i++)
	{
		// 0: unack'ed  , 1; ack'ed
		senderSrPacketAckStates[i] = 0;
	}

	thread senderSrThread(senderSR,sock);
	thread receiverThread(receiverMain, sock);
	thread senderAckThread(senderAckOnly, sock);

	string line;
	while(true)
	{
		getline(std::cin ,line);
		if(line == "BYE" ){
			fprintf(stderr, "END SIGNAL RECEIVED BY MAIN THREAD\n");
			break;
		}
		if(std::cin.eof()){
			fprintf(stderr, "End of message. SIGNAL RECEIVED BY MAIN THREAD\n");
			break;
		}
		// keep sending messages, split them into 8byte packages.
		int numOfPacketsNeeded = ceil(line.size() / 8.0);
		fprintf(stderr, "received inp: %s\n , size:%d , numPack: %d", line, line.size(), numOfPacketsNeeded);
		for(int i = 0; i < numOfPacketsNeeded; i++)
		{
			// split into packets of at most 8 bytes/chars.
			string chunk = line.substr(i*8,8);
			// printf("main waiting fo dataqmutex with chunk: %s\n", chunk);
			{
				printf("main waiting fo dataqmutex with chunk: %s\n", chunk);
				unique_lock<mutex>mlock(dataQueueMutex);
				Packet *p = (Packet *) malloc(sizeof(Packet));
				strncpy(p->contents, chunk.c_str(), 8);
				p->isAck = 0;
				p->seqNumber = sendNextSeqNum;	
				dataPacketSendQueue.push(p);

				sendNextSeqNum++;
				fprintf(stderr, "data q not empty SIGNALED by main\n");
				cvDataNotEmpty.notify_one();
			}
		}
	}
	senderAckThread.join();
	senderSrThread.join();
	senderAckThread.join();

	close(sock);

	return 0;
}

void senderAckOnly(int sock)
{
	int sendsock = sock;
	struct sockaddr_in addr;
	Packet sendbuffer;
	socklen_t len = sizeof(trolladdr);
	Packet* packet = nullptr;

	while(1)
	{
		{
			fprintf(stderr, "waiting on ack q mutex\n");
			std::unique_lock<std::mutex> mlock(ackQueueMutex);
			while(ackPacketSendQueue.empty())
			{
				fprintf(stderr,"waiting on ACK q not empty\n");
				cvAckNotEmpty.wait(mlock);
			}

			// get the package, copy over to our buffer
			packet = ackPacketSendQueue.front();
			ackPacketSendQueue.pop();
		}
	
		strncpy(sendbuffer.contents, packet->contents, 8);
		sendbuffer.isAck = packet->isAck;
		sendbuffer.seqNumber = packet->seqNumber;
		if(packet->isAck != 1){
			fprintf(stderr,"this is a data packet. should not be on ACK queue.\n");
		}

		free(packet);


		// fprintf(stderr, "sending ACK packet\n");
		int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
						(struct sockaddr *)&trolladdr, len);
		if (nsent<0) 
		{
			perror("server send response error\n");
			exit(1);
		}
	}
}
void senderSR(int sock)
{
	int sendsock = sock;//*((int *) vargs);
	struct sockaddr_in addr;
	Packet sendbuffer;
	socklen_t len = sizeof(trolladdr);

	Packet* packet = nullptr;

	while(1)
	{
		// get the package, copy over to our buffer
		{
			fprintf(stderr, "waiting on data Q mutex\n");
			std::unique_lock<std::mutex> mlock(dataQueueMutex);
			while(dataPacketSendQueue.empty())
			{
				fprintf(stderr, "waiting on data queue not empty\n");
				cvDataNotEmpty.wait(mlock);
			}
			packet = dataPacketSendQueue.front();
			dataPacketSendQueue.pop();

			strncpy(sendbuffer.contents, packet->contents, 8);
			// sendbuffer.contents = packet->contents;

			sendbuffer.isAck = packet->isAck;
			if(packet->isAck != 0){
				fprintf(stderr, "ACK packet no: %d should not be on data queue.\n", packet->seqNumber);
			}
			sendbuffer.seqNumber = packet->seqNumber;
			fprintf(stderr,"sending DATA packet\n");

			free(packet);
			int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
							(struct sockaddr *)&trolladdr, len);
			if (nsent<0) 
			{
				perror("server send response error");
				exit(1);
			}
		}
	}
}
void receiverMain(int sock)
{
	int rcvsock = sock; //*((int *) vargs);

	struct sockaddr_in mylocaladdr;
	Packet rcvBuffer;
	size_t rcvbufsize = sizeof rcvBuffer;
	socklen_t len = sizeof trolladdr;
	// Create and open a text file

	int rcvBaseSeqNum = 0;
  	ofstream chatOutput;

	Packet rcvOutOfOrderPacketBuffer[RCV_WINDOW_SIZE]; // in theory, there couldbe be at most windowSize-1 buffered packages., so 14 also works?
	for(int i = 0; i < RCV_WINDOW_SIZE; i++)
	{
		rcvOutOfOrderPacketBuffer[i].checksum = 0;
		strncpy(rcvOutOfOrderPacketBuffer[i].contents , "0000000", 8);
		rcvOutOfOrderPacketBuffer[i].seqNumber = -1;
		rcvOutOfOrderPacketBuffer[i].isAck = -1;
	}
	while(true)
	{
		// fprintf(stderr,"Server waiting for new message.\n");
		int n = recvfrom(rcvsock, (char *)&rcvBuffer, rcvbufsize, 0,
				(struct sockaddr *)&trolladdr, &len);
		if (n<0) {
			perror("server receiver recvfrom\n");
			exit(1);
		}

		// fprintf(stderr,"<<< incoming message content=%s\n", rcvBuffer.contents);
		// Check correctness via checksum, ignore/discard if faulty.
		if(rcvBuffer.isAck == 1)
		{
			// received ACK packet
			// do as Selective Repeat dictates for SenderSR.
			// mark packet as ack'ed, shift window size if possible etc.
			fprintf(stderr, "ReceiverSR received ACK packet: %d for our SenderSR.\n", rcvBuffer.seqNumber);
			int maxAcceptableSeqNum = sendNextSeqNum-1;
			if(rcvBuffer.seqNumber >= sendBaseSeqNum && rcvBuffer.seqNumber <= maxAcceptableSeqNum)
			{
				// TODO mark the packet as ack'ed here.
				senderSrPacketAckStates[rcvBuffer.seqNumber] = 1;

				if(rcvBuffer.seqNumber == sendBaseSeqNum)
				{
					// we can now advance the window
					// next base -> smallest un ack'ed seq num
					// this might require SYNC with SenderSR, as it reads base number at some point
					sendBaseSeqNum++; // TODO fix this
					// sendNextSeqNum++;
					// loop thru senderSrPacketAckStates and find the first 0? would that work?

					// let senderSR know that it can now send prev buffered(due to invalid seq num) packets if any.
					// fprintf(stderr, "Posting send window shift signal\n");
					// sem_post(&sendWindowShiftedSignal);
				}
			}
			else
			{
				// out of window size, why?
				fprintf(stderr, "[OUT OF WINDOW]ReceiverSR received ACK packet: %d for our SenderSR.\n", rcvBuffer.seqNumber);
			}
		}
		else if(rcvBuffer.isAck == 0)
		{
			// Received data packet, do as needed for ReceiverSR behaviour.
			int maxAcceptableSeqNum = rcvBaseSeqNum + (RCV_WINDOW_SIZE -1);
			if(rcvBuffer.seqNumber >= rcvBaseSeqNum && rcvBuffer.seqNumber <= maxAcceptableSeqNum)
			{
				{
					fprintf(stderr, "waiting on ackQmut to push ACK packet\n");

					unique_lock<mutex>mlock(ackQueueMutex);
					Packet *p = (Packet *) malloc(sizeof(Packet));
					strncpy(p->contents, "0000000", 8);
					p->isAck = 1;
					p->seqNumber = rcvBuffer.seqNumber;
					ackPacketSendQueue.push(p);

					fprintf(stderr, "Signaled ACK NOT EMPTY\n");
					cvAckNotEmpty.notify_one();
				}
				// check if received/buffered before
				bool alreadyBuffered = false;
				for(int j = 0; j < RCV_WINDOW_SIZE; j++)
				{
					if(rcvOutOfOrderPacketBuffer[j].seqNumber == rcvBuffer.seqNumber)
					{
						alreadyBuffered = true;
					}
				}
				if(!alreadyBuffered)
				{
					// buffer at the first available location.
					for(int i = 0; i < RCV_WINDOW_SIZE; i++)
					{
						if(rcvOutOfOrderPacketBuffer[i].seqNumber == -1) // marked as empty
						{
							// Buffer the packet content for later in-order delivery
							fprintf(stderr,"Buffer packet seqNo: %d and content: %s\n", rcvBuffer.seqNumber, rcvBuffer.contents);

							rcvOutOfOrderPacketBuffer[i].checksum = rcvBuffer.checksum;
							strncpy(rcvOutOfOrderPacketBuffer[i].contents, rcvBuffer.contents, 8);
							rcvOutOfOrderPacketBuffer[i].isAck = rcvBuffer.isAck;
							rcvOutOfOrderPacketBuffer[i].seqNumber = rcvBuffer.seqNumber;
							break;
						}
					}
				}
				else{
					fprintf(stderr,"Already buffered packet seqNo: %d and content: %s\n", rcvBuffer.seqNumber, rcvBuffer.contents);
				}
				// if in-order, deliver all buffered packets
				if(rcvBuffer.seqNumber == rcvBaseSeqNum)
				{
					// in order packet!
					// deliver all buffered packets by first sorting on seqNumber??
					fprintf(stderr,"In order packet with seqNum: %d and content: %s\n", rcvBuffer.seqNumber, rcvBuffer.contents);
					for(int i = 0; i < RCV_WINDOW_SIZE; i++)
					{
						for(int j = 0; j < RCV_WINDOW_SIZE; j++)
						{
							// Find current smallest packet. its number should be the baseSeqNumber.
							if(rcvOutOfOrderPacketBuffer[j].seqNumber == rcvBaseSeqNum)
							{
								// ,after that baseSeqNum+1 and so on.
								// deliver the current smallest packet, by writing to output file.
								Packet smallest = rcvOutOfOrderPacketBuffer[j];
								rcvOutOfOrderPacketBuffer[j].seqNumber = -1; // mark as deleted.

								fprintf(stderr, "Deliver packet seqNo: %d, content: %s\n", rcvBaseSeqNum, smallest.contents);
								// shift receive window
								rcvBaseSeqNum += 1;
								fprintf(stderr, "rcv base is now:%d\n", rcvBaseSeqNum);

								chatOutput.open(outFilename, std::ios_base::app); // append instead of overwrite
								chatOutput << smallest.contents;
								chatOutput.close();
								break;
							}
						}
					}
				}
				else
				{
					fprintf(stderr,"Out-of-order packet with seqNum: %d and content: %s, baseNum:%d\n", rcvBuffer.seqNumber, rcvBuffer.contents, rcvBaseSeqNum);
				}
			
			}
			else if(rcvBuffer.seqNumber >= (rcvBaseSeqNum - RCV_WINDOW_SIZE) && rcvBuffer.seqNumber <= rcvBaseSeqNum-1)
			{
				fprintf(stderr,"ACK'ing previously ACK'ed packet. seqNo: %d\n", rcvBuffer.seqNumber);
				// we have ACK'ed this before, but prob got lost. send ACK again.
				// send ACK

				{
					fprintf(stderr, "waiting on ackQmut to push ACK packet PREV ACK\n");
					unique_lock<mutex>mlock(ackQueueMutex);
					Packet *p = (Packet *) malloc(sizeof(Packet));
					strncpy(p->contents, "0000000", 8);
					p->isAck = 1;
					p->seqNumber = rcvBuffer.seqNumber;
					ackPacketSendQueue.push(p);
					fprintf(stderr, "ACKNOT EMPT SIGNALED\n");

					cvAckNotEmpty.notify_one();
				}
			}
			else
			{
				fprintf(stderr, "packet ignored seqNum: %d, rcvbase:%d\n", rcvBuffer.seqNumber, rcvBaseSeqNum);
			}
		}
		else
		{
			fprintf(stderr, "invalid isAck field: %d\n", rcvBuffer.isAck);
		}
	}
}