cmake_minimum_required(VERSION 3.5)
PROJECT(Immix)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(Immix
        src/datastructures/Bitmap.c
        src/datastructures/Bitmap.h
        src/datastructures/BlockList.c
        src/datastructures/BlockList.h
        src/datastructures/Stack.c
        src/datastructures/Stack.h
        src/headers/BlockHeader.h
        src/headers/LineHeader.h
        src/headers/ObjectHeader.h
        src/utils/MathUtils.h
        src/ThreadLocalAllocator.c
        src/ThreadLocalAllocator.h
        src/Block.c
        src/Block.h
        src/Constants.h
        src/GCTypes.h
        src/Heap.c
        src/Heap.h
        src/ImmixGC.c
        src/ImmixGC.h
        src/LargeAllocator.c
        src/LargeAllocator.h
        src/Line.h
        src/Log.h
        src/Marker.c
        src/Marker.h
        src/Memory.h
        src/Object.c
        src/Object.h
        src/StackoverflowHandler.c
        src/StackoverflowHandler.h
        src/StackTrace.h
        src/State.c
        src/State.h
        src/concurrent/GlobalBlockAllocator.h
        src/concurrent/GlobalBlockAllocator.cpp
        src/concurrent/GlobalLargeAllocator.h
        src/concurrent/GlobalLargeAllocator.cpp
        src/concurrent/MutatorSync.h
        src/concurrent/MutatorSync.cpp
        src/concurrent/MutatorThread.h
        src/concurrent/ValStack.h)

target_link_libraries(Immix PRIVATE Threads::Threads)
set_target_properties(Immix PROPERTIES ARCHIVE_OUTPUT_DIRECTORY build)