# This is used during the build so we always need to add it
ADD_SUBDIRECTORY(embedfile)

# Now add the optional tools
if (FSO_BUILD_TOOLS)
    ADD_SUBDIRECTORY(strings_tool)
    ADD_SUBDIRECTORY(comm_bundle_packager)
endif ()
