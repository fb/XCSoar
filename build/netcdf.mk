NETCDF = y

ifeq ($(TARGET),UNIX)
$(eval $(call pkg-config-library,NETCDF,netcdf-cxx4))
$(eval $(call link-library,netcdfcpp,NETCDF))
endif
LDLIBS += $(NETCDF_LDLIBS)
