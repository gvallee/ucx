#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strlen
#include <stdint.h>

struct desc_data {
	void *buf;
	size_t buf_size;
	uint32_t mkey;
	uint16_t vhca_id;
	char access_key[256];
	size_t access_key_sz;	
};

//========================================================================================================================================
/*                                    vhca_id mkey     buf_addr         buf_size access_key							 */
#define BUFF_DESC_STRING_LENGTH (sizeof "0102:01020304:0102030405060708:01020304:0102030405060708091011121314151617181920212223242526272829303132")

int
serialize_desc_data(struct desc_data *data, char* desc_str, size_t desc_length)
{
	int ret;
	if (desc_length < BUFF_DESC_STRING_LENGTH) {
		fprintf(stderr, "desc string size (%lu) is less than required (%lu) for sending cross gvmi attributes\n",
				desc_length, BUFF_DESC_STRING_LENGTH);
		return 0;
	}
	/*     vhca_id mkey     buf_addr	 buf_size
		 "0102:01020304:0102030405060708:01020304:" */
	ret = sprintf(desc_str, "%04x:%08x:%016llx:%08x:",
			data->vhca_id,
			data->mkey,
			(uint64_t)data->buf,
			data->buf_size);

	int i;
	for (i = 0; i < 32; ++i)
		sprintf(&desc_str[ret + i], "%c", data->access_key[i]);

	return strlen(desc_str) + 1; /*including the terminating null character*/
}

int
deserialize_desc_data(const char *str, size_t str_size, struct desc_data *data)
{
	/* ====================================================================================
         *vhca_id mkey     buff_addr        buf_size access_key
         *  "0102:01020304:0102030405060708:01020304:0102030405060708090a0b0c0d0e0f10"
	 **/
	sscanf(str, "%hx:%lx:%llx:%lx", &data->vhca_id, &data->mkey,
			&data->buf, &data->buf_size);
	data->access_key_sz = str_size - sizeof("0102:01020304:0102030405060708:01020304") - 1;
	memcpy(data->access_key, &str[sizeof("0102:01020304:0102030405060708:01020304")], data->access_key_sz);
	return 0;
}

int
sign_buffer(void *buf, size_t buf_size) {
	uint8_t *buffer = (uint8_t*)buf;
	if (buf_size < 4) {
		fprintf(stderr, "Buffer size is too small to sign.\n");
		return -1;
	}
	*(uint32_t *)&buffer[0] = htobe32((uint32_t)0xABCDEFFF);
	return 0;
}

int
verify_signature(void *buf, size_t buf_size) {
	uint8_t *buffer = (uint8_t*)buf;
	if (buf_size < 4) {
		fprintf(stderr, "Buffer size is too small to verify signature.\n");
		return -1;
	}
	printf("Buffer bits: %02x %02x %02x %02x\n",
		buffer[0], buffer[1], buffer[2], buffer[3]);
	if (buffer[0] != 0xAB ||
		buffer[1] != 0xCD ||
		buffer[2] != 0xEF ||
		buffer[3] != 0xFF) {
		fprintf(stderr, "Buffer is not contains the signature.\n");
		return -1;
	}

	printf("Buffer is verified.\n");

	return 0;
}
