# This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

# where to put generated libraries
set(LIBRARY_OUTPUT_PATH "${BUILD_DIR}/src/connector")

add_library(Connector STATIC
    connector.cpp
    hive_connector.cpp
)