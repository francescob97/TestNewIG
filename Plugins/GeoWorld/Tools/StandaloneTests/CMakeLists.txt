# =============================================================================
#  Harness di test STANDALONE per lo strato puro di GeoCore.
#
#  Non ha niente a che vedere con Unreal: compila direttamente i .cpp di
#  Public/Geo + Private/Geo con un normale compilatore C++ e li esercita.
#
#  Uso:
#     cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
#     cmake --build build
#     ./build/geocore_tests
#
#  Perche' esiste, visto che UE ha gia' il suo framework di automazione:
#   - gira in un secondo, senza aprire l'editor e senza una licenza di Windows;
#   - gira in CI su Linux;
#   - dimostra per costruzione che lo strato puro e' davvero puro: se qualcuno
#     ci infila un #include "CoreMinimal.h", questa build si rompe subito.
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(GeoCoreStandaloneTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(GEOCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoCore)

add_executable(geocore_tests
	main.cpp
	${GEOCORE_DIR}/Private/Geo/Ellipsoid.cpp
)

target_include_directories(geocore_tests PRIVATE ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geocore_tests PRIVATE /W4 /fp:precise)
else()
	# -ffast-math NON va usato: romperebbe le assunzioni IEEE su cui poggiano
	# le soglie numeriche di questi test (e la geodesia in generale).
	target_compile_options(geocore_tests PRIVATE -Wall -Wextra -O2)
endif()

# --- GeoTiles (Fase 3): formato delle tile, schema, cache LRU ---------------
set(GEOTILES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles)

add_executable(geotiles_tests
	geotiles_main.cpp
	${GEOTILES_DIR}/Private/Tiles/TilingScheme.cpp
	${GEOTILES_DIR}/Private/Tiles/TileFormat.cpp
)
target_include_directories(geotiles_tests PRIVATE
	${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geotiles_tests PRIVATE /W4)
else()
	target_compile_options(geotiles_tests PRIVATE -Wall -Wextra -O2)
endif()

# --- Quadtree (Fase 4): volumi, culling, selezione LOD ---------------------
set(GEORENDER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoRender)

add_executable(geoquadtree_tests
	geoquadtree_main.cpp
	${GEORENDER_DIR}/Private/Quadtree/Culling.cpp
	${GEORENDER_DIR}/Private/Quadtree/TileSelector.cpp
	${GEOTILES_DIR}/Private/Tiles/TilingScheme.cpp
	${GEOCORE_DIR}/Private/Geo/Ellipsoid.cpp
)
target_include_directories(geoquadtree_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geoquadtree_tests PRIVATE /W4)
else()
	target_compile_options(geoquadtree_tests PRIVATE -Wall -Wextra -O2)
endif()

enable_testing()
add_test(NAME geocore COMMAND geocore_tests)
add_test(NAME geotiles COMMAND geotiles_tests)
add_test(NAME geoquadtree COMMAND geoquadtree_tests)
