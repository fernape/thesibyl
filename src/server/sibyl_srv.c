/*
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "bsd-base64.h"

#define SIBYLPORT "9999"	// Sibyl default port
#define BACKLOG 20		// how many pending connections queue will hold
#define SIBYLDIR "/usr/local/sibyl"	// Sibyl keys and config dir
#define DECR_KEY "decrypt"
#define SIGN_KEY "sign"
#define FILE_LEN strlen("decrypt.pub")
#define SIBYL_MAX_MSG 65535

void sigchld_handler(int s)
{
	while(waitpid(-1, NULL, WNOHANG) > 0);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main (int argc, char *argv[])
{
	int status, sock, newsock;
	struct addrinfo hints, *srvinfo, *p;
	struct sockaddr_storage client_addr;
	socklen_t sin_size;
	struct sigaction sa;
	int yes = 1;
	char s[INET6_ADDRSTRLEN];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	/* RSA private keys */
	char *decr_fname, *sign_fname;
	decr_fname = (char *)calloc(_POSIX_PATH_MAX, sizeof(char));
	sign_fname = (char *)calloc(_POSIX_PATH_MAX, sizeof(char));
	if(decr_fname == NULL || sign_fname == NULL){
		perror("decr_fname or sign_fname alloc");
		exit(1);
	}

	if(strlen(SIBYLDIR) >= _POSIX_PATH_MAX ||
	   FILE_LEN >= _POSIX_PATH_MAX ||
	   (strlen(SIBYLDIR) + 1 + FILE_LEN) >= _POSIX_PATH_MAX){
		perror("dir lenght");
		exit(1);
	}

	// TODO: names should be configurable
	strncat(decr_fname, SIBYLDIR, _POSIX_PATH_MAX);
	strncat(decr_fname, "/decrypt", strlen("/decrypt"));
	strncat(sign_fname, SIBYLDIR, _POSIX_PATH_MAX);
	strncat(sign_fname, "/sign", strlen("/sign"));

	/* Fetch the private keys */
	FILE *decr_f, *sign_f;
	if((decr_f = fopen(decr_fname, "r")) == NULL){
		perror("Unable to alloate memory for decr_f");
		exit(1);
	}
	if((sign_f = fopen(sign_fname, "r")) == NULL){
		perror("Unable to allocate memory for sign_f");
		exit(1);
	}

	/* RSA *decrypt *sign */
	RSA *decrypt, *sign;
	decrypt = (RSA *) calloc(1, sizeof(RSA));
	sign = (RSA *) calloc(1, sizeof(RSA));
	if((decrypt = RSA_new()) == NULL){
		perror("Unable to RSA_new() decrypt");
		exit(1);
	}
	if((sign = RSA_new()) == NULL){
		perror("Unable to RSA_new() sign");
		exit(1);
	}


	/* Read the private keys */
	PEM_read_RSAPrivateKey(decr_f, &decrypt, NULL, NULL);
	PEM_read_RSAPrivateKey(sign_f, &sign, NULL, NULL);

	if(decrypt->n == NULL){
		perror("Unable to read the RSA decrypt key");
		fclose(decr_f);
		fclose(sign_f);
		exit(1);
	}
	if(sign->n == NULL){
		perror("Unable to read the RSA sign key");
		fclose(decr_f);
		fclose(sign_f);
		exit(1);
	}
	fclose(decr_f);
	fclose(sign_f);

	printf("Private keys read\n");

	/* Start listening */
	if ((status = getaddrinfo(NULL, SIBYLPORT, &hints, &srvinfo)) != 0) {
		fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for(p = srvinfo; p != NULL; p = p->ai_next){
		if ((sock = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1){
			perror("setsockopt");
			continue;
		}

  	if (bind(sock, p->ai_addr, p->ai_addrlen) == -1) {
			close(sock);
			perror("perror: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "server: failed to bind\n");
		return 2;
	}

	freeaddrinfo(srvinfo);

	if (listen(sock, BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}

	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1){
		perror("sigaction");
		exit(1);
	}

	printf("Waiting for connections...\n");

	while(1){
		sin_size = sizeof client_addr;
		newsock = accept(sock, (struct sockaddr *)&client_addr, &sin_size);
		if (newsock == -1){
			perror("accept");
			continue;
		}

		inet_ntop(client_addr.ss_family,
			  get_in_addr((struct sockaddr *)&client_addr), s, sizeof(s));
		printf("server: got connection from %s\n", s);

		if (!fork()){ // child process
			close(sock); // child doesn't need the listener

			// seed some bytes into the PRNG
			FILE *rand_f;
			int seed;
			struct timeval tv;
			if((rand_f = fopen("/dev/random", "r")) == NULL){
				gettimeofday(&tv,0);
				seed = tv.tv_sec + tv.tv_usec;
			} else {
				if(fread(&seed,sizeof(seed),1,rand_f) == 1) {
					fclose(rand_f);
				} else {
					gettimeofday(&tv,0);
					seed = tv.tv_sec + tv.tv_usec;
				}
			}
			RAND_seed((const void *)&seed, sizeof(seed));

			// generate a random nonce.
			u_char nonce[9];
			int count;
			char *strnonce;
			strnonce = (char *) calloc(17, sizeof(char));
			if (strnonce == NULL){
				perror("strnonce calloc");
				exit(1);
			}
			RAND_bytes(nonce, 8);
			for(count = 0; count < 8; count++)
				sprintf(strnonce+count*2, "%02X", nonce[count]);			
			// send the nonce
			if (send(newsock, strnonce, 17, 0) == -1){
				perror("send strnonce");
				exit(1);
			}

			// the nonce ends in '@'
			if (send(newsock, "@", 1, 0) == -1){
				perror("send '@'");
				exit(1);
			}

			/* receive the client's message */
			char *msg;
			msg = (char *) calloc(SIBYL_MAX_MSG, sizeof(char));
			if(msg == NULL){
				perror("Unable to allocate memory for the client's message");
				exit(1);
			}

			int count_bytes = 0;
			int bytes_rcvd = 0;
			while(count_bytes == 0 || 
			      !( (*(msg+(count_bytes-1)) == '\n') &&
				 (*(msg+(count_bytes-2)) == '@') &&
				 (*(msg+(count_bytes-3)) == '@') &&
				 (*(msg+(count_bytes-4)) == '\n'))){
				if((bytes_rcvd = recv(newsock, msg + count_bytes,
						      SIBYL_MAX_MSG - count_bytes, 0)) <= 0){
					printf("Connection error with the client. "
					       "received [%i] bytes through [%i], and "
					       "till now the message was [%s]",
					       bytes_rcvd, newsock, msg);
					exit(1);
				}
				if((count_bytes += bytes_rcvd) > SIBYL_MAX_MSG){
					perror("Sibyl's client is sending more bytes than"
					       "necessary");
					exit(1);
				}
			}

			printf("Received: [%s]\n", msg);

			/* parse message, which is as follows: 
			 * m;p1;p2\n@@\n
 			 * where
 			 * m	nonce generated by the client, usually different from
 			 * 	the one generated by us (the sibyl).
 			 * p1	the entry in the 'shadow' file corresponding to
 			 * 	the user
 			 * p2	is the output of 
 			 * 	b64_encode(RSA_encrypt("nonce:crypt(passwd,salt)"))
 			 * 	where nonce is the nonce generated by the sibyl
 			 * the trailing \n@@\n is the end-of-message notification
 			 */

			/* remove the end-of-message notification */
			char *new_msg = (char *)malloc(count_bytes-4);
			strncpy(new_msg, msg, count_bytes-4);

			char *token[3];
			/* token[0] = m */
			token[0] = strsep(&new_msg, ";");
			if(token[0] == NULL){
				perror("Malformed message received from the client");
				exit(1);
			}
			/* token[1] = p1 */
			token[1] = strsep(&new_msg, ";");
			if(token[1] == NULL){
				perror("Malformed message received from the client");
				exit(1);
			}
			/* token[2] = p2 */
			token[2] = strsep(&new_msg, ";");
			if(token[2] == NULL){
				perror("Malformed message received from the client");
				exit(1);
			}
			/* there should not be more tokens */
			if(strsep(&new_msg, ";") != NULL){
				perror("Malformed message received from the client");
				exit(1);
			}

			printf("m : %s\n", token[0]);
			printf("p1 : %s\n", token[1]);
			printf("p2 : %s\n", token[2]);

			/* decrypt p1 (p1 = token[1]) */
			int rsa_d;
			char *p1_rsa = (char *)malloc(RSA_size(decrypt) + 1);
			b64_pton(token[1],
				 (u_char *)p1_rsa,
				 RSA_size(decrypt) + 1);

			char *p1_data = (char *)calloc(RSA_size(decrypt) + 1, sizeof(u_char));

			rsa_d = RSA_private_decrypt(RSA_size(decrypt),
						    (u_char *)p1_rsa,
						    (u_char *)p1_data,
						    decrypt,
						    RSA_PKCS1_OAEP_PADDING);

			if (rsa_d == -1){
				perror("p1 RSA decryption error");
				exit(1);
			}

			printf("p1_data: %s\n", p1_data);

			/* decrypt p2 (p2 = token[2]) */
			char *p2_rsa = (char *)malloc(RSA_size(decrypt) + 1);
			b64_pton(token[2],
				 (u_char *)p2_rsa,
				 RSA_size(decrypt) + 1);


			char *p2_data = (char *)calloc(RSA_size(decrypt) + 1, sizeof(u_char));

			rsa_d = RSA_private_decrypt(RSA_size(decrypt),
						    (u_char *)p2_rsa,
						    (u_char *)p2_data,
						    decrypt,
						    RSA_PKCS1_OAEP_PADDING);

			if (rsa_d == -1){
				perror("p2 RSA decryption error");
				exit(1);
			}

			printf("p2_data: %s\n", p2_data);

			/* Calculates v1, that is: p2_data = n:v1 */
			char *p2_token[2];
			/* p2_token[0] = nonce */
			p2_token[0] = strsep(&p2_data, ":");
			if(p2_token[0] == NULL){
				perror("Malformed p2_data");
				exit(1);
			}
			/* p2_token[1] = v1 */
			p2_token[1] = strsep(&p2_data, ":");
			if(p2_token[1] == NULL){
				perror("Malformed p2_data");
				exit(1);
			}

			printf("nonce: %s\n", p2_token[0]);
			printf("v1: %s\n", p2_token[1]);

			/* Is the password correct? */
			char *auth_result = calloc(1, sizeof(char));
			if((strcmp(p1_data, p2_token[1]) == 0) && 
			   (strcmp(strnonce, p2_token[0]) == 0)){
				auth_result = "1";
				printf("auth ok\n");
			} else {
				auth_result = "0";
				printf("auth NOok\n");
			}

			/* Create the response, which is as follows:
 			 * M;signature
 			 * where M is a message (text without semicolons)
 			 * 	M has the following structure:
 			 * 	M = n:X
 			 * 	where
 			 * 	n is the nonce received from the client before
 			 * 	X is either '0' or '1', for 'Not authenticated' or 'Authenticated'
 			 * signature is the RSA signature of M
 			 * 	actually (b64_encode(signature))
 			 *
 			 * */
			char *message;
			message = (char *) calloc(strlen(token[0]) + 1 + 
						  strlen(auth_result), sizeof(char));
			if (message == NULL){
				perror("Unable to allocate memory for message");
				exit(1);
			}
			// TODO: use strncat instead of strcat with a NONCE_LENGHT const
			strcat(message, token[0]);
			strcat(message, ":");
			strcat(message, auth_result);

			printf("message: %s\n", message);

			/* computes the SHA-1 message digest (20 bytes) */
			char *sha1_m = (char *) calloc(20, sizeof(char));
			if (sha1_m == NULL){
				perror("Unable to allocate memory for sha1_m");
				exit(1);
			}
			SHA1((u_char *)message, strlen(message), (u_char*)sha1_m); 

			/* sign the message digest */
			char *signature = (char *) malloc(RSA_size(sign) + 1);
			if (signature == NULL){
				perror("Unable to allocate memory for signature");
				exit(1);
			}
			u_int siglen;
			siglen = RSA_size(sign);
			if (RSA_sign(NID_sha1,
				     (u_char *)sha1_m,
				     20,
				     (u_char *)signature,
				     &siglen,
				     sign) != 1){
				perror("Error signing the message");
				exit(1);
			}

			/* encode the signature to base-64 */
			char *signature_b64 = (char *)malloc(RSA_size(sign) * 4);
			b64_ntop((u_char *)signature,
				 RSA_size(sign),
				 signature_b64,
				 RSA_size(sign) * 4);

			printf("signature_b64: %s\n", signature_b64);

			/* creates the response string */
			char *response;
			response = (char *) calloc(strlen(message) + 1 +
						   strlen(signature_b64), sizeof(char));
			if (response == NULL){
				perror("Unable to allocate memory for response");
				exit(1);
			}
			strcat(response, message);
			strcat(response, ";");
			strcat(response, signature_b64);

			printf("response: %s\n", response);

			/* Send response */
			if (send(newsock, response, strlen(response), 0) == -1){
				perror("send response");
				exit(1);
			}

			/* Close socket */
			close(newsock);
			exit(0);
		}
		close(newsock); // parent doesn't need this
	}

	return 0;
}
