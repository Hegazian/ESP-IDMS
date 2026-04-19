idf_component_register(
  SRCS "onewire.c" "ds18b20.c" "xpt2046.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES driver
)
