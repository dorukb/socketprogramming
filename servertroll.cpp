/*
Parts of this are taken from fromtroll.c and totroll.c, which are written by:
 * Mitchell Tasman
 * December 1987
 * Modified by Marvin Solomon, October 1989.
and some parts are taken from getaddrinfo(7) man page.
*//*

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

const string outFilename("serverOut.txt");

void senderAckOnly(int s);
void senderSR(int s);
void receiverMain(int s);
void Timers(int sock);

typedef struct Packet {
	char isAck;
	int seqNumber;
	long checksum;
	char contents[8];
} Packet;

typedef struct Timer{
    chrono::steady_clock::time_point timeSent;
	Packet* packet;
} Timer;

// readonly globals?
struct sockaddr_in trolladdr;

// Global vars that need sync
queue<Packet*> dataPacketSendQueue;
mutex dataQueueMutex;
std::condition_variable cvDataNotEmpty;

queue<Packet*> ackPacketSendQueue;
mutex ackQueueMutex;
std::condition_variable cvAckNotEmpty;

std::condition_variable cvSendWindowShifted;
mutex mSendWindowShifted;

vector<Timer> timers;
mutex timersMutex;
// since this is both sender & receiver, we have two sets of variables for Selective Repeat.
long sendBaseSeqNum = 0;
long sendNextSeqNum = 0;

#define SEND_WINDOW_SIZE 15
#define RCV_WINDOW_SIZE 15
#define SEND_SEQ_NUM_SPACE 50
#define RCV_SEQ_NUM_SPACE 50

// send seq num space client = receive num space of server
// rcv seqnum space of client = send num space of server!

int senderSrPacketAckStates[SEND_SEQ_NUM_SPACE];

uint16_t Fletcher16(uint8_t *data, int count)
{
   uint16_t sum1 = 0;
   uint16_t sum2 = 0;

   for (int i = 0; i < count; i++)
   {
      sum1 = (sum1 + data[i]) % 255;
      sum2 = (sum2 + sum1) % 255;
   }
   return (sum2 << 8) | sum1;
}

int main(int argc, char *argv[])
{
	int sock;	/* a socket for receiving responses */ 

	/* process arguments */
	if (argc != 2) {
		fprintf(stderr, "usage: %s port\n", argv[0]);
		exit(1);
	}

	u_short port = atoi(argv[1]);
	if (port < 1024 || port > 0xffff) {
		fprintf(stderr, "%s: Bad troll port %d (must be between 1024 and %d)\n",
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
		perror("server bind");
		exit(1);
	}
	for(int i = 0 ; i < SEND_WINDOW_SIZE; i++)
	{
		// 0: unack'ed  , 1; ack'ed
		senderSrPacketAckStates[i] = 0;
	}

  	thread senderSrThread(senderSR,sock);
	thread receiverThread(receiverMain, sock);
	thread senderAckThread(senderAckOnly, sock);
	thread timersThread(Timers, sock);

	string line;
	while(true)
	{
		getline(std::cin ,line);
		if(line == "BYE" ){
			fprintf(stderr, "END SIGNAL RECEIVED BY MAIN THREAD\n");
			break;
		}
		if(std::cin.eof()){
			fprintf(stderr, "End of message. seqNum:%d  data:%s\n\n\n\n", sendNextSeqNum, line);
			break;
		}
		// keep sending messages, split them into 8byte packages.
		int numOfPacketsNeeded = ceil(line.size() / 8.0);
		fprintf(stderr, "received inp: %s\n , size:%d , numPack: %d", line, line.size(), numOfPacketsNeeded);
		for(int i = 0; i < numOfPacketsNeeded; i++)
		{
			// split into packets of at most 8 bytes/chars.
			string chunk = line.substr(i*8,8);

			// Check if nextSeqNumber is available to send.

			{
				unique_lock<mutex>windowlock(mSendWindowShifted);
				if(sendNextSeqNum >= sendBaseSeqNum && sendNextSeqNum <= sendBaseSeqNum+SEND_WINDOW_SIZE-1)
				{
					// can send yes,
					// otherwise must wait for a shift.
				}
				else{				
					fprintf(stderr, "main waiting for WINDOW SHIFT, nextSeqNum: %d\n", sendNextSeqNum);
					cvSendWindowShifted.wait(windowlock);
				}
			}
			// printf("main waiting fo dataqmutex with chunk: %s\n", chunk);
			{
				printf("main waiting fo dataqmutex with chunk: %s\n", chunk);
				unique_lock<mutex>mlock(dataQueueMutex);

				// Here we need to wait if data packet send queue is FULL.
				// cvDataQueueNotFull.wait() etc.
				Packet *p = (Packet *) malloc(sizeof(Packet));
				strncpy(p->contents, chunk.c_str(), 8);
				p->isAck = 0;
				p->seqNumber = sendNextSeqNum;	
				p->checksum = Fletcher16((uint8_t*)p->contents, 8);
				fprintf(stderr, "Checksum: %ld seqNo;%d\n", p->checksum, p->seqNumber);

				dataPacketSendQueue.push(p);

				sendNextSeqNum = (sendNextSeqNum+1) % SEND_SEQ_NUM_SPACE;
				fprintf(stderr, "data q not empty SIGNALED by main\n");
				cvDataNotEmpty.notify_one();
			}
		}
	}
	senderAckThread.join();
	senderSrThread.join();
	senderAckThread.join();
	timersThread.join();


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
		sendbuffer.checksum = packet->checksum;
		if(packet->isAck != 1){
			fprintf(stderr,"this is a data packet. should not be on ACK queue.\n");
		}
		int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
							(struct sockaddr *)&trolladdr, len);
		if (nsent<0) 
		{
			perror("server send response error\n");
		}
		
		// // register this packet with timers
		// {
		// 	unique_lock<mutex> timerlock(timersMutex);
		// 	// fprintf(stderr, "sending ACK packet\n");
		// 	Timer timer;
		// 	timer.packet = packet;
		// 	timer.timeSent = chrono::steady_clock::now();
		// 	timers.push_back(timer);
		// }
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
			// fprintf(stderr, "waiting on data Q mutex\n");
			{
				std::unique_lock<std::mutex> mlock(dataQueueMutex);
				while(dataPacketSendQueue.empty())
				{
					fprintf(stderr, "waiting on data queue not empty\n");
					cvDataNotEmpty.wait(mlock);
				}
				packet = dataPacketSendQueue.front();
				dataPacketSendQueue.pop();
		
				strncpy(sendbuffer.contents, packet->contents, 8);
				sendbuffer.isAck = packet->isAck;
				if(packet->isAck != 0){
					fprintf(stderr, "ACK packet no: %d should not be on data queue.\n", packet->seqNumber);
				}
				sendbuffer.seqNumber = packet->seqNumber;
				sendbuffer.checksum = packet->checksum;

				// free(packet);
			}
			
			// senderSrPacketAckStates
			// mark this packet as sent, but not yet ACK'ed and start its timer.
			{
				std::unique_lock<std::mutex> ackStateLock(mSendWindowShifted);
				senderSrPacketAckStates[sendbuffer.seqNumber] = 0;
			}
			// register this packet with timers
			{
				unique_lock<mutex> timerlock(timersMutex);
				// fprintf(stderr, "sending ACK packet\n");
				Timer timer;
				timer.packet = packet;
				timer.timeSent = chrono::steady_clock::now();
				timers.push_back(timer);
				int nsent = sendto(sendsock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
							(struct sockaddr *)&trolladdr, len);
				if (nsent<0) 
				{
					perror("server send response error");
					// exit(1);
				}
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
		long calculatedChecksum = Fletcher16((uint8_t*)rcvBuffer.contents, 8);
		if(calculatedChecksum != rcvBuffer.checksum){
			fprintf(stderr, "Checksums dont match. calc:%ld send:%ld seqNo:%d", calculatedChecksum, rcvBuffer.checksum, rcvBuffer.seqNumber);
			continue;
		}

		if(rcvBuffer.isAck == 1)
		{
			// received ACK packet
			// do as Selective Repeat dictates for SenderSR.
			// mark packet as ack'ed, shift window size if possible etc.
			// fprintf(stderr, "ReceiverSR received ACK packet: %d for our SenderSR.\n", rcvBuffer.seqNumber);
			int maxAcceptableSeqNum = sendBaseSeqNum + SEND_WINDOW_SIZE-1;
			if(rcvBuffer.seqNumber >= sendBaseSeqNum && rcvBuffer.seqNumber <= maxAcceptableSeqNum)
			{

					// Stop its timer.
					{
						unique_lock<mutex>(timersMutex);
						auto indexToRemove = timers.end();
						// remove from the timer's list.
						for(auto it = timers.begin() ; it < timers.end(); it++){
							if((*it).packet->seqNumber == rcvBuffer.seqNumber)
							{
								indexToRemove = it;

								fprintf(stderr,"[DELIVERY CONFIRMED] seqNo: %d\n",it->packet->seqNumber);
								free(it->packet);
								break;
							}
						}
						if(indexToRemove != timers.end())
						{
							timers.erase(indexToRemove);
						}
					}
					// Then mark as acked and move on.
					{
						unique_lock<mutex>(mSendWindowShifted);
						// mark the packet as ack'ed here.
						senderSrPacketAckStates[rcvBuffer.seqNumber] = 1;
						if(rcvBuffer.seqNumber == sendBaseSeqNum)
						{
							// we can now advance the window
							int newBaseSeqNum = sendBaseSeqNum;
							// next base -> smallest un ack'ed seq num
							for(int i = sendBaseSeqNum; i < sendBaseSeqNum + SEND_WINDOW_SIZE; i++)
							{
								if(senderSrPacketAckStates[i] == 1){
									// this one is ack'ed move one step.
									newBaseSeqNum = (newBaseSeqNum +1) % SEND_SEQ_NUM_SPACE;
								}
								else{
									break;
								}
							}
							sendBaseSeqNum = newBaseSeqNum;					
							fprintf(stderr, "after ack'ing no:%d , new send_base:%d\n", rcvBuffer.seqNumber, sendBaseSeqNum);

							// Now need to transmit buffered packages if their seq numbers are in window
							// Now signal data sender, main thread that it can continue sending data, as the window shifted. new seq numbers are available.
							cvSendWindowShifted.notify_all();
							// let senderSR know that it can now send prev buffered(due to invalid seq num) packets if any.
						}
						else
						{
							// out of window size, why?
							fprintf(stderr, "[OUT OF WINDOW]ReceiverSR received ACK packet: %d for our SenderSR.\n", rcvBuffer.seqNumber);
						}
					}


			}
			
		}
		else if(rcvBuffer.isAck == 0)
		{
			// Received data packet, do as needed for ReceiverSR behaviour.
			int maxAcceptableSeqNum = rcvBaseSeqNum + (RCV_WINDOW_SIZE -1);
			int lowerLimit = rcvBaseSeqNum - RCV_WINDOW_SIZE;

			bool alternate = false;
			if(lowerLimit < 0) {
				alternate = true;
				lowerLimit += RCV_SEQ_NUM_SPACE;
			}
			int upperLimit = rcvBaseSeqNum -1;
			if(upperLimit < 0){
				alternate= true;
				upperLimit += RCV_SEQ_NUM_SPACE;
			}

			if(rcvBuffer.seqNumber >= rcvBaseSeqNum && rcvBuffer.seqNumber <= maxAcceptableSeqNum)
			{
				{
					// fprintf(stderr, "waiting on ackQmut to push ACK packet\n");

					unique_lock<mutex>mlock(ackQueueMutex);
					Packet *p = (Packet *) malloc(sizeof(Packet));
					strncpy(p->contents, "0000000", 8);
					p->isAck = 1;
					p->seqNumber = rcvBuffer.seqNumber;
					p->checksum = Fletcher16((uint8_t*)p->contents, 8);

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
								// rcvBaseSeqNum += 1;
								rcvBaseSeqNum = (rcvBaseSeqNum+1) % RCV_SEQ_NUM_SPACE;
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
			else if(!alternate && rcvBuffer.seqNumber >= lowerLimit && rcvBuffer.seqNumber <= upperLimit)
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
					p->checksum = Fletcher16((uint8_t*)p->contents, 8);

					ackPacketSendQueue.push(p);
					fprintf(stderr, "ACKNOT EMPT SIGNALED\n");

					cvAckNotEmpty.notify_one();
				}
			}
			else if(alternate && (rcvBuffer.seqNumber >= lowerLimit || rcvBuffer.seqNumber <= upperLimit)){
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
					p->checksum = Fletcher16((uint8_t*)p->contents, 8);

					ackPacketSendQueue.push(p);
					fprintf(stderr, "ACKNOT EMPT SIGNALED\n");

					cvAckNotEmpty.notify_one();
				}
			}
			else
			{
				fprintf(stderr, "packet ignored seqNum: %d, rcvbase:%d lower: %d  upper:%d\n", rcvBuffer.seqNumber, rcvBaseSeqNum,
				lowerLimit, upperLimit);
			}
		}
		else
		{
			fprintf(stderr, "invalid isAck field: %d\n", rcvBuffer.isAck);
		}
	}
}


void Timers(int sock)
{
	// 50 ms timer for each.
	int64_t timeout = 50;

	Packet sendbuffer;	
	socklen_t len = sizeof(trolladdr);

	while(1)
	{
		{
			unique_lock<mutex> mlock(timersMutex);
			for(int i = 0; i < timers.size(); i++)
			{
				chrono::steady_clock::time_point begin = chrono::steady_clock::now();
				chrono::steady_clock::time_point currTime = chrono::steady_clock::now();
				auto timePassed = chrono::duration_cast<std::chrono::milliseconds>(currTime - timers[0].timeSent).count();
				if(timePassed > timeout)
				{
					// re transmit these packets.
					strncpy(sendbuffer.contents, timers[i].packet->contents, 8);
					sendbuffer.isAck = timers[i].packet->isAck;
					sendbuffer.seqNumber = timers[i].packet->seqNumber;
					sendbuffer.checksum = timers[i].packet->checksum;

					// POSSIBLE ERROR POINT, sync sendto() calls!
					int nsent = sendto(sock, (char *)&sendbuffer, sizeof(sendbuffer), 0,
						(struct sockaddr *)&trolladdr, len);
					if (nsent<0) 
					{
						perror("Timer retransmit packet error");
					}
					// restart this timer as well.
					timers[i].timeSent = chrono::steady_clock::now();
				}
			}
		}
		std::this_thread::sleep_for(10ms);
	}
}