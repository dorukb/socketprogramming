/*

Parts of this are taken from fromtroll.c and totroll.c, which are written by:
 * Mitchell Tasman
 * December 1987
 * Modified by Marvin Solomon, October 1989.
and some parts are taken from getaddrinfo(7) man page.
*/
#include <sys/syscall.h>
#include <pthread.h>
#include <semaphore.h>
#include <queue>
#include <stdlib.h>
#include <string.h> 
#include <unistd.h>
#include <string>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <fstream>
#include <iostream>

using namespace std;

void *senderAckOnly(void *vargs);
void *senderSR(void *vargs);
void *receiverMain(void *vargs);

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
sem_t dataQueueMutex; // to protect dataPacketSendQueue

queue<Packet*> ackPacketSendQueue;
sem_t ackQueueMutex;

// since this is both sender & receiver, we have two sets of variables for Selective Repeat.
long sendBaseSeqNum = 0;
long sendNextSeqNum = 0;

#define SEND_WINDOW_SIZE 15
#define RCV_WINDOW_SIZE 15

int senderSrPacketAckStates[SEND_WINDOW_SIZE];

Packet rcvOutOfOrderPacketBuffer[RCV_WINDOW_SIZE]; // in theory, there couldbe be at most windowSize-1 buffered packages., so 14 also works?
sem_t timersMutex; // to protect timers? for resending

sem_t DataQueueNotEmptySignal; // to signal there is a data packet to send.
sem_t AckQueueNotEmptySignal;	// to signal there is an ACK packet to send.
sem_t sendWindowShiftedSignal;	// to signal send window shifted, for SenderSR

int main(int argc, char *argv[])
{
	int sock;	/* a socket for sending messages*/
	Packet message;
	struct hostent *host;
	struct sockaddr_in localaddr;
	fd_set selectmask;
	int counter, n;

	printf("argc: %d\n", argc);

	// 0 is execname, 1 is IP , 2 is troll port, 3 is local port.

	/* get troll address and port ... */

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

    sem_init(&dataQueueMutex, 0, 1);
    sem_init(&ackQueueMutex, 0, 1);
    sem_init(&timersMutex, 0, 1);

	sem_init(&DataQueueNotEmptySignal, 0, 0);
	sem_init(&AckQueueNotEmptySignal, 0, 0);
	sem_init(&sendWindowShiftedSignal, 0, 0);

	for(int i = 0 ; i < SEND_WINDOW_SIZE; i++)
	{
		// 0: unack'ed  , 1; ack'ed
		senderSrPacketAckStates[i] = 0;
	}
	for(int i = 0; i < RCV_WINDOW_SIZE; i++)
	{
		rcvOutOfOrderPacketBuffer[i].seqNumber = -1;
		rcvOutOfOrderPacketBuffer[i].isAck = -1;
	}
    pthread_t senderSRThread = pthread_t();
	pthread_t senderAckThread = pthread_t();
	pthread_t receiverThread = pthread_t();

	int *arg = (int*)malloc(sizeof(int));
	*arg = sock;
	int *arg2 = (int*)malloc(sizeof(int));
	*arg2 = sock;
	int *arg3 = (int*)malloc(sizeof(int));
	*arg3 = sock;

	pthread_create(&senderSRThread, NULL, senderSR, arg);
	pthread_create(&receiverThread, NULL, receiverMain, arg2);
	pthread_create(&senderAckThread, NULL, senderAckOnly, arg3);


	string line;
	while(true)
	{
		getline(std::cin ,line);
		if(line == "BYE"){
			fprintf(stderr, "END SIGNAL RECEIVED BY MAIN THREAD\n");
			break;
		}
		// keep sending messages, split them into 8byte packages.
		int numOfPacketsNeeded = ceil(line.size() / 8.0);
		fprintf(stderr, "received inp: %s\n , size:%d , numPack: %d", line, line.size(), numOfPacketsNeeded);
		for(int i = 0; i < numOfPacketsNeeded; i++)
		{
			// split into packets of at most 8 bytes/chars.
			string chunk = line.substr(i*8,8);
			// push to data queue here

			// printf("main waiting fo dataqmutex with chunk: %s\n", chunk);

			sem_wait(&dataQueueMutex);
			Packet *p = (Packet *) malloc(sizeof(Packet));
			strncpy(p->contents, chunk.c_str(), 8);
			p->isAck = 0;
			p->seqNumber = sendNextSeqNum;	
			dataPacketSendQueue.push(p);

			sendNextSeqNum++;
			if(dataPacketSendQueue.size() == 1)
			{
				// Wake up SenderSR thread to send this data packet.
				// fprintf(stderr, "data q not empty SIGNALED by main\n");
				sem_post(&DataQueueNotEmptySignal);
			}
			sem_post(&dataQueueMutex);
		}
	}
	pthread_join(senderSRThread, NULL);
	pthread_join(receiverThread, NULL);
	pthread_join(senderAckThread, NULL);

	close(sock);

	return 0;
}

void *senderAckOnly(void *vargs)
{
	int sendsock = *((int *) vargs);
	struct sockaddr_in addr;
	Packet sendbuffer;
	socklen_t len = sizeof(trolladdr);
	Packet* packet = nullptr;

	while(1)
	{
		// fprintf(stderr, "waiting on ack q mutex\n");
		sem_wait(&ackQueueMutex);
		if(ackPacketSendQueue.empty())
		{
			sem_post(&ackQueueMutex);

			// fprintf(stderr,"waiting on ACK q not empty\n");
			sem_wait(&AckQueueNotEmptySignal);

			sem_wait(&ackQueueMutex);
			// fprintf(stderr, "ACK q not empty received\n");

		}

		// get the package, copy over to our buffer
		packet = ackPacketSendQueue.front();
		ackPacketSendQueue.pop();
		strncpy(sendbuffer.contents, packet->contents, 8);
		sendbuffer.isAck = packet->isAck;
		sendbuffer.seqNumber = packet->seqNumber;

		if(packet->isAck != 1){
			fprintf(stderr,"this is a data packet. should not be on ACK queue.\n");
		}

		// fprintf(stderr, "sending ACK packet\n");
		int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
						(struct sockaddr *)&trolladdr, len);
		if (nsent<0) 
		{
			perror("server send response error\n");
			sem_post(&ackQueueMutex);
			exit(1);
		}
		sem_post(&ackQueueMutex);
	}
}
void *senderSR(void *vargs)
{
	int sendsock = *((int *) vargs);
	struct sockaddr_in addr;
	Packet sendbuffer;
	socklen_t len = sizeof(trolladdr);

	Packet* packet = nullptr;

	while(1)
	{
		fprintf(stderr, "waiting on data Q mutex\n");
		sem_wait(&dataQueueMutex);
		
		if(dataPacketSendQueue.empty())
		{
			sem_post(&dataQueueMutex);
			fprintf(stderr, "waiting on data queue not empty\n");
			sem_wait(&DataQueueNotEmptySignal);

			sem_wait(&dataQueueMutex);
			fprintf(stderr, "data queue not empty received\n");
		}

		// get the package, copy over to our buffer

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
		int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
						(struct sockaddr *)&trolladdr, len);
		if (nsent<0) 
		{
			perror("server send response error");
			sem_post(&dataQueueMutex);
			exit(1);
		}
		sem_post(&dataQueueMutex);
	}
}
void *receiverMain(void *vargs)
{
	int rcvsock = *((int *) vargs);

	struct sockaddr_in mylocaladdr;
	Packet rcvBuffer;
	size_t rcvbufsize = sizeof rcvBuffer;
	socklen_t len = sizeof trolladdr;
	// Create and open a text file

  	ofstream chatOutput;
	long rcvBaseSeqNum = 0;

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
					sem_post(&sendWindowShiftedSignal);
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
			int maxAcceptableSeqNum = rcvBaseSeqNum + RCV_WINDOW_SIZE -1;
			if(rcvBuffer.seqNumber >= rcvBaseSeqNum && rcvBuffer.seqNumber <= maxAcceptableSeqNum)
			{
				// send ACK
				sem_wait(&ackQueueMutex);
				Packet *p = (Packet *) malloc(sizeof(Packet));
				strncpy(p->contents, "0000000", 8);
				p->isAck = 1;
				p->seqNumber = rcvBuffer.seqNumber;
				ackPacketSendQueue.push(p);
				if(ackPacketSendQueue.size() == 1)
				{
					// Signal sendAckOnly thread that there is now a ACK package to send.
					sem_post(&AckQueueNotEmptySignal);
				}
				sem_post(&ackQueueMutex);

				// check if received/buffered before
				bool alreadyBuffered = false;
				for(int i = 0; i < RCV_WINDOW_SIZE; i++)
				{
					if(rcvOutOfOrderPacketBuffer[i].seqNumber == rcvBuffer.seqNumber)
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
							rcvOutOfOrderPacketBuffer[i].checksum = rcvBuffer.checksum;
							strncpy(rcvOutOfOrderPacketBuffer[i].contents, rcvBuffer.contents, 8);
							rcvOutOfOrderPacketBuffer[i].isAck = rcvBuffer.isAck;
							rcvOutOfOrderPacketBuffer[i].seqNumber = rcvBuffer.seqNumber;
						}
					}
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
								rcvBaseSeqNum += 1;
								chatOutput.open("clientOutput.txt", std::ios_base::app); // append instead of overwrite
								chatOutput << smallest.contents;
								chatOutput.close();
								break;
							}
						}
					}
				}
				else
				{
					fprintf(stderr,"Out-of-order packet with seqNum: %d and content: %s\n", rcvBuffer.seqNumber, rcvBuffer.contents);
				}
			
			}
			else if(rcvBuffer.seqNumber >= (rcvBaseSeqNum - RCV_WINDOW_SIZE) && rcvBuffer.seqNumber <= rcvBaseSeqNum-1)
			{
				fprintf(stderr,"ACK'ing previously ACK'ed packet. seqNo: %d\n", rcvBuffer.seqNumber);
				// we have ACK'ed this before, but prob got lost. send ACK again.
				// send ACK
				sem_wait(&ackQueueMutex);
				Packet *p = (Packet *) malloc(sizeof(Packet));
				strncpy(p->contents, "0000000", 8);
				p->isAck = 1;
				p->seqNumber = rcvBuffer.seqNumber;
				ackPacketSendQueue.push(p);
				if(ackPacketSendQueue.size() == 1)
				{
					// Signal sendAckOnly thread that there is now a ACK package to send.
					sem_post(&AckQueueNotEmptySignal);
				}
				sem_post(&ackQueueMutex);

			}
			else
			{
				fprintf(stderr, "packet ignored seqNum: %d rcvbase:%d\n", rcvBuffer.seqNumber, rcvBaseSeqNum);
			}
		}
		else
		{
			fprintf(stderr, "invalid isAck field: %d\n", rcvBuffer.isAck);
		}
	}
}