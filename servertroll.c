/*
 * Mitchell Tasman
 * December 1987
 * Modified by Marvin Solomon, October 1989.
 * Program fromtroll.c
 *
 * Testing program to test the "troll" (q.v.)
 * Listens for messages and display the messages 
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

void *senderMain(void *vargs);
void *receiverMain(void *vargs);

int errno;
typedef struct MyMessage {
	char isAck;
	long contents;
} MyMessage;

/* for lint */
void bzero(), bcopy(), exit(), perror();
#define Printf (void)printf
#define Fprintf (void)fprintf



u_short port;
struct sockaddr_in trolladdr;

main(argc,argv)
int argc;
char *argv[];
{
	int sock;	/* a socket for receiving responses */
	MyMessage message;
	message.isAck = 0;

	int n;
	int lastseq = -1;

	MyMessage ackMsg;
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
	char sendQ[2000]; // queue for outgoing packages.
	int sendIndex = 0; 

    pthread_t senderThread = malloc(sizeof(pthread_t));
	pthread_t receiverThread = malloc(sizeof(pthread_t));

	pthread_create(&senderThread, NULL, senderMain, NULL);
	pthread_create(&receiverThread, NULL, receiverMain, NULL);
	sem_t sendQMutex;
	sem_t timersMutex;
    sem_init(&sendQMutex, 0, 1);
    sem_init(&timersMutex, 0, 1);

	int msgCount  = 0;
	for(;;) {
		int len = sizeof trolladdr;

		/* read in one message from the troll */
		n = recvfrom(sock, (char *)&message, sizeof message, 0,
				(struct sockaddr *)&trolladdr, &len);
		if (n<0) {
			perror("fromtroll recvfrom");
			exit(1);
		}
		n = message.contents - (lastseq+1);
		msgCount++;
		if (n == 0)
		{
			Printf("<<< incoming message content=%d  send ack response.\n", message.contents);
			ackMsg.contents = msgCount;
			int nsent = sendto(sock, (char *)&ackMsg, sizeof ackMsg, 0,
					 (struct sockaddr *)&trolladdr, sizeof trolladdr);
			if (nsent<0) {
				perror("server send response error");
				exit(1);
			}
		}
		else if (n > 0)
			Printf("<<< incoming message content=%d (%d missing)\n", message.contents, n);
		else
			Printf("<<< incoming message content=%d (duplicate)\n", message.contents);
		lastseq = message.contents;

		errno = 0;
	}

	pthread_join(senderThread, NULL);
}

// How to use threading for nonblocking send&receive?
//  you can set up two threads (sender & receiver)
//   then have the sender send everything from a queue
//    (and use a mutex for the producer/consumer relationship between the sender and the receiver), 


void *senderMain(void *vargs)
{
	int sendsock;
	struct sockaddr_in addr;

	if ((sendsock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("sendermain send socket creation error");
		exit(1);
	}

	/* ... and bind its local address */
	bzero((char *)&addr, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY; /* let the kernel fill this in */
	addr.sin_port = htons(port);
	if (bind(sendsock, (struct sockaddr *)&addr, sizeof addr) < 0) {
		perror("client bind");
		exit(1);
	}

}
void *receiverMain(void *vargs)
{

}