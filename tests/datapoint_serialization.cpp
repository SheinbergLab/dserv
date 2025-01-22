#include <iostream>
#include <stdio.h>
#include <string.h>

#include "Datapoint.h"

// Perhaps we should adopt Google Test, with handy assertions, for tests like this.
// https://google.github.io/googletest/quickstart-cmake.html

int main(int argc, char *argv[])
{
    // Construct a sample datapoint.
    char varname[] = "test/datapoint";
    uint64_t timestmap = 42;
    double data = 1234.4567;
    ds_datapoint_t *datapoint = dpoint_new(
        varname,
        timestmap,
        DSERV_DOUBLE,
        sizeof(double),
        (unsigned char *)&data);

    // By convention, datapoint varlen should be the length of varname, plus 1.
    // This way the buffer includes the null terminator.
    if(datapoint->varlen != strlen(varname) + 1)
        exit(1);
    if (strcmp(datapoint->varname, varname))
        exit(1);

    // Serialize, then deserialize the datapoint.
    int buffer_size = dpoint_binary_size(datapoint);
    unsigned char *buffer = (unsigned char *)malloc(buffer_size);
    int serialized_size = dpoint_to_binary(datapoint, buffer, &buffer_size);

    if (buffer_size != serialized_size)
        exit(1);

    ds_datapoint_t *datapoint_2 = dpoint_from_binary((char *)buffer, serialized_size);


    // The deserialized datapoint should match the original.
    if (strcmp(datapoint_2->varname, varname))
        exit(1);

    if(datapoint_2->varlen != strlen(varname) + 1)
        exit(1);

    if(datapoint_2->timestamp != timestmap)
        exit(1);

    if(datapoint_2->data.type != DSERV_DOUBLE)
        exit(1);

    if(datapoint_2->data.len != sizeof(double))
        exit(1);

    double data_2 = *(double *)(datapoint_2->data.buf);
    if(data_2 != data)
        exit(1);

    free(buffer);
    dpoint_free(datapoint_2);
    dpoint_free(datapoint);
}
