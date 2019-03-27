/*
Author: James Zhang
Notes: Using skeleton code from Beej's Guide to Network Programming
sender.c -- a datagram sockets "server" demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include "packetTransfer.h"

#define MAXBUFLEN 1024
#define HEADERSIZE 16
#define WAITS 2
#define WAITU 10000 //if WAITS is 0, WAITU must be reasonably large (such as at least 10000)

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;
	struct sockaddr_storage their_addr;
	char buf[MAXBUFLEN+HEADERSIZE];
	socklen_t addr_len;
	char s[INET6_ADDRSTRLEN];
	int cwnd;
	double pl, pc;

	// extract argument parameters
	if (argc != 5)
	{
		fprintf(stderr,"usage: receiver <portnumber> <CWnd> <Pl> <Pc>\n");
		exit(1);
	}

	char *port = argv[1];
	cwnd = atoi(argv[2]);
	pl = atof(argv[3]);
	pc = atof(argv[4]);

	if(pl < 0 || pc < 0 || pl > 1 || pc > 1)
	{
		fprintf(stderr,"Pl and Pc must be between 0 and 1!\n");
		exit(1);
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // set to AF_INET to force IPv4
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next)
	{
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			perror("sender: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
		{
			close(sockfd);
			perror("sender: bind");
			continue;
		}

		break;
	}

	if (p == NULL)
	{
		fprintf(stderr, "Error: sender failed to bind socket\n");
		return 2;
	}

	freeaddrinfo(servinfo);

	printf("Waiting for requested filename...\n");

	addr_len = sizeof their_addr;

	if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN-1 , 0, (struct sockaddr *)&their_addr, &addr_len)) == -1)
	{
		printf("Error: failed to receive filename\n");
		exit(1);
	}

	printf("Received filename from %s\n", inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s));
	buf[numbytes] = '\0';
	printf("Requested filename: \"%s\"\n", buf);

	// buf contains requested file name; if it exists, send back packets (up to 1 KB at a time) to receiver

	// create source buffer to fopen and fread designated file
	char *source = NULL;
	char sourcePacket[MAXBUFLEN+HEADERSIZE];
	FILE *fp = fopen(buf, "r");
	size_t sourceLength;

	if (fp==NULL)
	{
		// send any packet with seq=0 and fin=1 to represent no file found
		send2(sockfd, buf, strlen(buf), (struct sockaddr *)&their_addr, addr_len, 0, 0, 1, 0, 0, 0);
		printf("Error: file not found!\n");
		exit(1);
	}

	if (fseek(fp, 0L, SEEK_END) == 0)
	{
		// set fsize to file size
	        long fsize = ftell(fp);

	        if (fsize == -1)
		{
			// send any packet with seq=0 and fin=1 to represent no file found
			send2(sockfd, buf, strlen(buf), (struct sockaddr *)&their_addr, addr_len, 0, 0, 1, 0, 0, 0);
			printf("Error: file size error!\n");
			exit(1);
		}

		// allocate source buffer to filesize
		source = malloc(sizeof(char) * (fsize + 1));

		// return to front of file
		if (fseek(fp, 0L, SEEK_SET) != 0)
		{
			// send any packet with seq=0 and fin=1 to represent no file found
			send2(sockfd, buf, strlen(buf), (struct sockaddr *)&their_addr, addr_len, 0, 0, 1, 0, 0, 0);
			free(source);
			printf("Error: file size error!\n");
			exit(1);
		}

		// set source to file data
		sourceLength = fread(source, sizeof(char), fsize, fp);

		// check file source for fread errors
		if (sourceLength == 0)
		{
			// send any packet with seq=0 and fin=1 to represent no file found
			send2(sockfd, buf, strlen(buf), (struct sockaddr *)&their_addr, addr_len, 0, 0, 1, 0, 0, 0);
			free(source);
			printf("Error: file reading error!\n");
			exit(1);
		}

		// NULL-terminate the source
		source[sourceLength] = '\0';
	}

	// close file
	fclose(fp);

	// initialize variables for sending/receiving acks
	int seq=0;
	int ack=0;
	size_t len=0;
	short fin=0;
	short crc=0;
	int expectedAck=0;

	int windowPosition=0;
	int tempPosition=0;

	while(fin!=1)
	{
		int numPacketsSent = 0;
		tempPosition = windowPosition;

		// check that more packets need to be sent and we have not exceeded our send window
		while(tempPosition<sourceLength && numPacketsSent<cwnd)
		{
			// determine size (since it may be less than MAXBUFLEN if last packet to be sent)
			int size = MAXBUFLEN;
			if(sourceLength-tempPosition<MAXBUFLEN)
				size = sourceLength-tempPosition;

			// create temp array holding data to be sent
			char temp[MAXBUFLEN];
			bzero(temp, MAXBUFLEN);
			memcpy(temp, source+tempPosition, size);

			// send packet with headers and data
			numbytes = send2(sockfd, temp, size, (struct sockaddr *)&their_addr, addr_len, tempPosition, seq+len, (tempPosition+MAXBUFLEN>=sourceLength), 0, pl, pc);

//printf("Sent packet of %d-bytes\n", size);

			if (numbytes == -1)
			{
				printf("Packet with sequence #%d lost!\n", tempPosition);
			}
			else if(numbytes == -2)
			{
				printf("Packet with sequence #%d corrupted!\n", tempPosition);
			}
			else
				printf("Sent %d bytes with sequence #%d\n", numbytes, tempPosition);

			// increment position of data to send
			tempPosition+=MAXBUFLEN;
			
			// increment number of packets sent this round
			numPacketsSent++;
		}

		int numAcksReceived=0;

		// check that acks of packets sent have been received
		while(numAcksReceived < numPacketsSent)
		{
			// initialize variables
			fd_set inSet;
			struct timeval timeout;
			int received;

			FD_ZERO(&inSet);
			FD_SET(sockfd, &inSet); 

			// wait for specified time
			timeout.tv_sec = WAITS;
			timeout.tv_usec = WAITU;

			// check for acks
			received = select(sockfd+1, &inSet, NULL, NULL, &timeout);

			// if timeout and no acks were received, break and maintain window position
			if(received < 1)
			{
				printf("Timed out while waiting for ACK!\n");
				break;
			}

			// otherwise, fetch the ack
			numbytes = receive2(sockfd, buf, &len, (struct sockaddr *)&their_addr, &addr_len, &seq, &ack, &fin, &crc);

			//printf("got ack with: %d/%d/%d/%d (s/a/f/c)\n", seq, ack, fin, crc);
		
			if(fin == 1)
			{
				printf("ACK #%d received!\n", ack);

				// FIN rides on the back of the last data packet
				printf("FIN sent!\n");

				// FINACK is guaranteed to arrive; packet loss/corruption disabled
				printf("FINACK received!\n");
				break;
			}
			// ignore ack if it's corrupted (we define this as crc == 1); break and maintain window position
			if(crc == 1)
			{
				printf("ACK #%d is corrupted! Ignoring it.\n", ack);
				break;
			}

			// ack is the one we were expecting, so we can increment windowPosition and expect the next ack
			// true if ack is at most expectedAck and more than expectedAck-MAXBUFLEN
			if(ack > expectedAck && ack <= expectedAck + MAXBUFLEN)
			{
				printf("ACK #%d received!\n", ack);
				numAcksReceived++;
				expectedAck += MAXBUFLEN; // expect the next ack (assume it's a MAXBUFLEN-byte packet)
				windowPosition += MAXBUFLEN;
			}

		}

		// at this point, if we didn't receive the expected ACK, continue
	}	

	printf("File finished transmitting! Sender terminating...\n");

	// free file source and close socket
	free(source);
	close(sockfd);

	return 0;
}

