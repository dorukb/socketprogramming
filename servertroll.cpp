/*
 * Mitchell Tasman
 * December 1987
 * Modified by Marvin Solomon, October 1989.
 * Program fromtroll.c
 *
 * Testing program to test the "troll" (q.v.)
 * Listens for messages and display the messages 
 */
#include <sys/syscall.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <queue>
#include <stdlib.h>
#include <string.h> 
#include <condition_variable>

using namespace std;

void *senderMain(void *vargs);
void *receiverMain(void *vargs);



// int errno;
typedef struct Packet {
	char isAck;
	int seqNumber;
	long contents;
} Packet;

/* for lint */
// void bzero(), bcopy(), perror();
#define Printf (void)printf
#define Fprintf (void)fprintf

// readonly globals?
u_short port;
struct sockaddr_in trolladdr;

// Global vars that need sync
std::mutex cvMutex;
std::condition_variable cvEmpty;

sem_t sendQMutex;
sem_t timersMutex;
// char sendQ[2000]; // queue for outgoing packages.
// int sendIndex = 0; 

queue<Packet*> sendpacketq;

int main(int argc, char *argv[])
{
	int sock;	/* a socket for receiving responses */
	Packet message;
	message.isAck = 0;

	int n;
	int lastseq = -1;

	Packet ackMsg;
	ackMsg.isAck = 1; // set ACK bit
	ackMsg.contents = 1998;

	/* process arguments */
	if (argc != 2) {
		Fprintf(stderr, "usage: %s port\n", argv[0]);
		exit(1);
	}

	port = atoi(argv[1]);
	if (port < 1024 || port > 0xffff) {
		Fprintf(stderr, "%s: Bad troll port %d (must be between 1024 and %d)\n",
			argv[0], port, 0xffff);
		exit(1);
	}

	/* create a socket... */

	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("fromtroll socket");
		exit(1);
	}

	struct sockaddr_in localaddr;
	/* ... and bind its local address */
	bzero((char *)&localaddr, sizeof localaddr);
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = INADDR_ANY; /* let the kernel fill this in */
	localaddr.sin_port = htons(port);
	if (bind(sock, (struct sockaddr *)&localaddr, sizeof localaddr) < 0) {
		perror("client bind");
		exit(1);
	}

	/* Main loop */
    sem_init(&sendQMutex, 0, 1);
    sem_init(&timersMutex, 0, 1);

    pthread_t senderThread = pthread_t();//(pthread_t *) malloc(sizeof(pthread_t));
	pthread_t receiverThread = pthread_t();//(pthread_t *) malloc(sizeof(pthread_t));

	pthread_create(&senderThread, NULL, senderMain, NULL);
	pthread_create(&receiverThread, NULL, receiverMain, NULL);
 

	// int msgCount  = 0;
	// for(;;) {
	// 	socklen_t len = sizeof trolladdr;

	// 	/* read in one message from the troll */
	// 	n = recvfrom(sock, (char *)&message, sizeof message, 0,
	// 			(struct sockaddr *)&trolladdr, &len);
	// 	if (n<0) {
	// 		perror("fromtroll recvfrom");
	// 		exit(1);
	// 	}
	// 	n = message.contents - (lastseq+1);
	// 	msgCount++;
	// 	if (n == 0)
	// 	{
	// 		Printf("<<< incoming message content=%d  send ack response.\n", message.contents);
	// 		ackMsg.contents = msgCount;
	// 		int nsent = sendto(sock, (char *)&ackMsg, sizeof ackMsg, 0,
	// 				 (struct sockaddr *)&trolladdr, sizeof trolladdr);
	// 		if (nsent<0) {
	// 			perror("server send response error");
	// 			exit(1);
	// 		}
	// 	}
	// 	else if (n > 0)
	// 		Printf("<<< incoming message content=%d (%d missing)\n", message.contents, n);
	// 	else
	// 		Printf("<<< incoming message content=%d (duplicate)\n", message.contents);
	// 	lastseq = message.contents;

	// 	errno = 0;
	// }

	pthread_join(senderThread, NULL);
	pthread_join(receiverThread, NULL);
	return 0;
}

// How to use threading for nonblocking send&receive?
//  you can set up two threads (sender & receiver)
//   then have the sender send everything from a queue
//    (and use a mutex for the producer/consumer relationship between the sender and the receiver), 


void *senderMain(void *vargs)
{
	int sendsock;
	struct sockaddr_in addr;
	Packet sendbuffer;
	socklen_t len = sizeof(trolladdr);

	if ((sendsock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("sendermain send socket creation error");
		exit(1);
	}

	/* ... and bind its local address */
	bzero((char *)&addr, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY; /* let the kernel fill this in */
	addr.sin_port = htons(port+1);
	if (bind(sendsock, (struct sockaddr *)&addr, sizeof addr) < 0) {
		perror("client bind error on sender main");
		exit(1);
	}

	std::unique_lock<std::mutex> lck(cvMutex);
	Packet* packet = nullptr;

	while(1)
	{
		sem_wait(&sendQMutex);
		
		if(sendpacketq.empty())
		{
			sem_post(&sendQMutex);
 			cvEmpty.wait(lck);
			sem_wait(&sendQMutex);
		}

		// get the package, copy over to our buffer
		packet = sendpacketq.front();
		sendpacketq.pop();

		sendbuffer.contents = packet->contents;
		sendbuffer.isAck = packet->isAck;
		sendbuffer.seqNumber = packet->seqNumber;
		fprintf(stderr, "sending packet");
		int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
						(struct sockaddr *)&trolladdr, len);
		if (nsent<0) 
		{
			perror("server send response error");
			sem_post(&sendQMutex);

			exit(1);
		}
		sem_post(&sendQMutex);
	}
}
void *receiverMain(void *vargs)
{
	int rcvsock;
	struct sockaddr_in mylocaladdr;
	Packet rcvBuffer;
	Packet ackMsg;
	size_t rcvbufsize = sizeof rcvBuffer;
	socklen_t len = sizeof trolladdr;

	if ((rcvsock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		perror("receiver main send socket creation error");
		exit(1);
	}

	/* ... and bind its local address */
	bzero((char *)&mylocaladdr, sizeof mylocaladdr);
	mylocaladdr.sin_family = AF_INET;
	mylocaladdr.sin_addr.s_addr = INADDR_ANY; /* let the kernel fill this in */
	mylocaladdr.sin_port = htons(port);

	if (bind(rcvsock, (struct sockaddr *)&mylocaladdr, sizeof mylocaladdr) < 0) {
		perror("server receiver bind");
		exit(1);
	}

	int msgCount = -1;	
	int lastseq = -1;


	std::unique_lock<std::mutex> lck(cvMutex);
	for(;;)
	{
		/* read in one message from the troll */
		Printf("Server waiting new message.\n");
		int n = recvfrom(rcvsock, (char *)&rcvBuffer, rcvbufsize, 0,
				(struct sockaddr *)&trolladdr, &len);
		if (n<0) {
			perror("server receiver recvfrom");
			exit(1);
		}

		n = rcvBuffer.contents - (lastseq+1);
		msgCount++;	
		if (n == 0)
		{
			Printf("<<< incoming message content=%d  send ack response.\n", rcvBuffer.contents);
			ackMsg.contents = msgCount;

			// push to sendQ here
			// int nsent = sendto(sock, (char *)&ackMsg, sizeof ackMsg, 0,
			// 		 (struct sockaddr *)&trolladdr, sizeof trolladdr);
			sem_wait(&sendQMutex);
			Packet *p = (Packet *) malloc(sizeof(Packet));
			p->contents = msgCount;
			p->isAck = 1;
			p->seqNumber = msgCount;

			sendpacketq.push(p);
			sem_post(&sendQMutex);
			
			// notify that there is a packet to send now.
			cvEmpty.notify_one();
		
		}
		else if (n > 0)
			Printf("<<< incoming message content=%d (%d missing)\n", rcvBuffer.contents, n);
		else
			Printf("<<< incoming message content=%d (duplicate)\n", rcvBuffer.contents);
		lastseq = rcvBuffer.contents;

	}
}