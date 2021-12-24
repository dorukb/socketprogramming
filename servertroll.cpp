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
#include <unistd.h>

using namespace std;

void *senderMain(void *vargs);
void *receiverMain(void *vargs);

typedef struct Packet {
	char isAck;
	int seqNumber;
	long contents;
} Packet;

#define Printf (void)printf
#define Fprintf (void)fprintf

// readonly globals?
u_short port;
struct sockaddr_in trolladdr;

// Global vars that need sync
queue<Packet*> sendpacketq;

sem_t sendQMutex; // to protect send packet queue
sem_t timersMutex; // to protect timers for resending

sem_t notEmptySignal; // to signal send packet queue not empty state.


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

	int optval = 1;
	setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof(int));

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

    sem_init(&sendQMutex, 0, 1);
    sem_init(&timersMutex, 0, 1);
	sem_init(&notEmptySignal, 0, 0);

    pthread_t senderThread = pthread_t();
	pthread_t receiverThread = pthread_t();

	int *arg = (int*)malloc(sizeof(*arg));
	*arg = sock;
	int *arg2 = (int*)malloc(sizeof(*arg));
	*arg2 = sock;
	pthread_create(&senderThread, NULL, senderMain, arg);
	pthread_create(&receiverThread, NULL, receiverMain, arg2);

	pthread_join(senderThread, NULL);
	pthread_join(receiverThread, NULL);

	close(sock);

	return 0;
}

void *senderMain(void *vargs)
{
	int sendsock = *((int *) vargs);
	struct sockaddr_in addr;
	Packet sendbuffer;
	socklen_t len = sizeof(trolladdr);

	Packet* packet = nullptr;

	while(1)
	{
		Printf("waiting on Q mutex\n");

		sem_wait(&sendQMutex);
		
		if(sendpacketq.empty())
		{
			sem_post(&sendQMutex);
			Printf("waiting on q not empty\n");
			sem_wait(&notEmptySignal);

			sem_wait(&sendQMutex);
			Printf(" not empty received\n");

		}

		// get the package, copy over to our buffer
		packet = sendpacketq.front();
		sendpacketq.pop();

		sendbuffer.contents = packet->contents;
		sendbuffer.isAck = packet->isAck;
		sendbuffer.seqNumber = packet->seqNumber;
		Printf("sending packet\n");
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
	int rcvsock = *((int *) vargs);

	struct sockaddr_in mylocaladdr;
	Packet rcvBuffer;
	Packet ackMsg;
	size_t rcvbufsize = sizeof rcvBuffer;
	socklen_t len = sizeof trolladdr;

	int msgCount = -1;	
	int lastseq = -1;

	for(;;)
	{
		Printf("Server waiting for new message.\n");
		int n = recvfrom(rcvsock, (char *)&rcvBuffer, rcvbufsize, 0,
				(struct sockaddr *)&trolladdr, &len);
		if (n<0) {
			perror("server receiver recvfrom\n");
			exit(1);
		}

		n = rcvBuffer.contents - (lastseq+1);
		msgCount++;	
		if (n == 0)
		{
			Printf("<<< incoming message content=%d  push  ack response packet on queue\n", rcvBuffer.contents);
			ackMsg.contents = msgCount;

			// push to sendQ here
			sem_wait(&sendQMutex);
			Packet *p = (Packet *) malloc(sizeof(Packet));
			p->contents = msgCount;
			p->isAck = 1;
			p->seqNumber = msgCount;

			sendpacketq.push(p);
			sem_post(&sendQMutex);
			
			sem_post(&notEmptySignal);
		
		}
		else if (n > 0)
			Printf("<<< incoming message content=%d (%d missing)\n", rcvBuffer.contents, n);
		else
			Printf("<<< incoming message content=%d (duplicate)\n", rcvBuffer.contents);
		lastseq = rcvBuffer.contents;

	}
}