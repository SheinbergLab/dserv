/*
 * dservapi
 *
 * API for sending datapoints to dataserver using either:
 *   %setdata
 * or
 *   BINARY_MESSAGE (update 128bytes)
 */

#include <Datapoint.h>

int dservapi_open_socket(const char *host, int port);
void dservapi_close_socket(int fd);
int dservapi_get_from_dataserver(int fd,
				 const char *varname,
				 char **buf);
int dservapi_write_to_dataserver(int fd,
				 const char *varname,
				 int dtype,
				 int len, void *data);
int dservapi_send_to_dataserver(int fd, const char *var,
				int dtype, int n, void *data);

