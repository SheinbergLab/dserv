#include <gtest/gtest.h>
#include <stdio.h>
#include <string.h>

#include <iostream>

#include "Datapoint.h"

TEST(DatapointSerialization, RoundTrip) {
  // Construct a sample datapoint.
  char varname[] = "test/datapoint";
  uint64_t timestmap = 42;
  double data = 1234.4567;
  ds_datapoint_t *datapoint = dpoint_new(
      varname, timestmap, DSERV_DOUBLE, sizeof(double), (unsigned char *)&data);

  // By convention, datapoint varlen should be the length of varname, plus 1.
  // This way the buffer includes the null terminator.
  EXPECT_EQ(datapoint->varlen, strlen(varname) + 1);
  EXPECT_STREQ(datapoint->varname, varname);

  // Serialize, then deserialize the datapoint.
  int buffer_size = dpoint_binary_size(datapoint);
  unsigned char *buffer = (unsigned char *)malloc(buffer_size);
  int serialized_size = dpoint_to_binary(datapoint, buffer, &buffer_size);

  dpoint_free(datapoint);

  EXPECT_EQ(buffer_size, serialized_size);

  ds_datapoint_t *datapoint_2 =
      dpoint_from_binary((char *)buffer, serialized_size);

  free(buffer);

  // The deserialized datapoint should match the original.
  EXPECT_EQ(datapoint_2->varlen, strlen(varname) + 1);
  EXPECT_STREQ(datapoint_2->varname, varname);
  EXPECT_EQ(datapoint_2->timestamp, timestmap);
  EXPECT_EQ(datapoint_2->data.type, DSERV_DOUBLE);
  EXPECT_EQ(datapoint_2->data.len, sizeof(double));

  double data_2 = *(double *)(datapoint_2->data.buf);
  EXPECT_EQ(data_2, data);

  dpoint_free(datapoint_2);
}
