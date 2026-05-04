/*
CRC.C
Sunday, March 5, 1995 6:21:30 PM
*/

//#include "cseries.h"

#include "crc.h"

/* ---------- constants */

enum
{
	CRC_NEW= 0xFFFFFFFF,
	CRC_TABLE_SIZE= 256,
	CRC32_POLYNOMIAL= 0xEDB88320L
};

/* ---------- globals */

static Boolean crc_table_initialized= FALSE;
static unsigned long crc_table[CRC_TABLE_SIZE];

/* ---------- private prototypes */

static void initialize_crc_table(void);

/* ---------- code */

void crc_new(
	unsigned long *crc_reference)
{
	*crc_reference= CRC_NEW;

	return;
}

void crc_checksum_buffer(
	unsigned long *crc_reference, 
	void *buffer,
	long count)
{
	unsigned long crc= *crc_reference;
	unsigned long a;
	unsigned long b;

	if (!crc_table_initialized) initialize_crc_table();

	while (count--) 
	{
		a= (crc>>8) & 0x00FFFFFFL;
		b= crc_table[(crc ^ *((char *)buffer)++) & 0xff];
		crc= a^b;
	}

	*crc_reference= crc;

	return;
}

/* ---------- private code */

static void initialize_crc_table(
	void)
{
	/* Build the table */
	short index, j;
	unsigned long crc;

	for(index= 0; index<CRC_TABLE_SIZE; ++index)
	{
		crc= index;
		for(j=0; j<8; j++)
		{
			if(crc & 1) crc=(crc>>1) ^ CRC32_POLYNOMIAL;
			else crc>>=1;
		}
		crc_table[index] = crc;
	}
	
	return;
}
