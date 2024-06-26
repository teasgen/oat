#include "oat.hpp"

//#define OSMIUM_WITH_TIMER

//#define OSMIUM_AREA_WITH_GEOS
#ifdef OSMIUM_AREA_WITH_GEOS
# include <geos/geom/MultiPolygon.h>
# include <geos/operation/valid/IsValidOp.h>
#endif

//#define WITH_OLD_STYLE_MP_SUPPORT

#ifdef WITH_OLD_STYLE_MP_SUPPORT
# include <osmium/area/assembler_legacy.hpp>
# include <osmium/area/multipolygon_manager_legacy.hpp>
#else
# include <osmium/area/assembler.hpp>
# include <osmium/area/multipolygon_manager.hpp>
#endif

#include <osmium/area/problem_reporter_ogr.hpp>
#include <osmium/area/problem_reporter_stream.hpp>
#include <osmium/geom/ogr.hpp>
#include <osmium/handler/dump.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/dense_mem_array.hpp>
#include <osmium/index/map/dense_mmap_array.hpp>
#include <osmium/index/map/dummy.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/index/map/sparse_mem_array.hpp>
#include <osmium/index/map/sparse_mmap_array.hpp>
#include <osmium/index/node_locations_map.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/util/memory.hpp>
#include <osmium/util/verbose_output.hpp>
#include <osmium/visitor.hpp>

#include <gdalcpp.hpp>

#include <cstdlib>
#include <getopt.h>
#include <iostream>

using index_type = osmium::index::map::Map<osmium::unsigned_object_id_type, osmium::Location>;
using location_handler_type = osmium::handler::NodeLocationsForWays<index_type>;

REGISTER_MAP(osmium::unsigned_object_id_type, osmium::Location, osmium::index::map::Dummy, none)

class OutputOGR : public osmium::handler::Handler {

    osmium::geom::OGRFactory<>& m_factory;

    gdalcpp::Layer m_layer_multipolygons;

    bool m_check = false;
    bool m_only_invalid = false;
    bool m_output_areas = false;

    static void print_area_error(const osmium::Area& area, const osmium::geometry_error& e) {
        std::cerr << "Ignoring illegal geometry for area "
                  << area.id()
                  << " created from "
                  << (area.from_way() ? "way" : "relation")
                  << " with id="
                  << area.orig_id() << " (" << e.what() << ").\n";
    }

public:

    OutputOGR(gdalcpp::Dataset& dataset, osmium::geom::OGRFactory<>& factory) :
        m_factory(factory),
        m_layer_multipolygons(dataset, "areas", wkbMultiPolygon, {"SPATIAL_INDEX=NO"}) {
        m_layer_multipolygons.add_field("id", OFTInteger, 10);
        m_layer_multipolygons.add_field("valid", OFTInteger, 1);
        m_layer_multipolygons.add_field("source", OFTString, 1);
        m_layer_multipolygons.add_field("orig_id", OFTInteger, 10);
    }

    void set_check(bool check) noexcept {
        m_check = check;
    }

    void set_only_invalid(bool only_invalid) noexcept {
        m_only_invalid = only_invalid;
    }

    void set_output_areas(bool output_areas) noexcept {
        m_output_areas = output_areas;
    }

    void area(const osmium::Area& area) {
        try {
            bool is_valid = false;
            auto geom = m_factory.create_multipolygon(area);
            if (m_check) {
#ifdef OSMIUM_AREA_WITH_GEOS
                auto geosgeom = geom->exportToGEOS();
                geos::operation::valid::IsValidOp ivo(reinterpret_cast<const geos::geom::Geometry *>(geosgeom));
                ivo.setSelfTouchingRingFormingHoleValid(true);
                is_valid = ivo.isValid();
                if (!is_valid) {
                    auto error = ivo.getValidationError();
                    std::cerr << "GEOS ERROR: " << error->toString() << '\n';
                }
#else
                is_valid = geom->IsValid();
#endif
            }
            if (m_only_invalid && is_valid) {
                return;
            }
            if (m_output_areas) {
                gdalcpp::Feature feature{m_layer_multipolygons, std::move(geom)};
                feature.set_field("id", static_cast<int32_t>(area.id()));
                feature.set_field("valid", is_valid);
                feature.set_field("source", area.from_way() ? "w" : "r");
                feature.set_field("orig_id", static_cast<int32_t>(area.orig_id()));
                feature.add_to_layer();
            }
        } catch (const osmium::geometry_error& e) {
            print_area_error(area, e);
        }
    }

}; // class OutputOGR


void print_help() {
    std::cout << "oat_create_areas [OPTIONS] OSMFILE\n\n"
              << "Read OSMFILE and build multipolygons from it.\n"
              << "\nOptions:\n"
              << "  -a, --suppress-area-output   Suppress output of created areas\n"
              << "  -c, --check                  Check geometries\n"
              << "  -C, --collect-only           Only collect data, don't assemble areas\n"
              << "  -f, --only-invalid           Filter out valid geometries\n"
              << "  -d, --debug[=LEVEL]          Set area assembler debug level\n"
              << "  -D, --dump-areas[=FILE]      Dump areas to file (default: stdout, also needs -o)\n"
              << "  -e, --empty-areas            Create empty areas for broken geometries\n"
              << "  -h, --help                   This help message\n"
              << "  -i, --index=INDEX_TYPE       Set index type for location index (default: flex_mem)\n"
              << "  -I, --show-index-types       Show available index types for location index\n"
              << "  -o, --output=DBNAME          Database name\n"
              << "  -O, --overwrite              Overwrite existing database\n"
              << "  -p, --report-problems[=FILE] Report problems to file (default: stdout)\n"
              << "  -r, --show-incomplete        Show incomplete relations\n"
              << "  -R, --check-roles            Check tagged member roles\n"
#ifdef WITH_OLD_STYLE_MP_SUPPORT
              << "  -s, --no-new-style           Do not output new style multipolygons\n"
              << "  -S, --no-old-style           Do not output old style multipolygons\n"
#else
              << "  -s, --no-new-style           Do not output multipolygons created from relations\n"
#endif
              << "  -t, --keep-type-tag          Keep type tag from mp relation (default: false)\n"
              << "  -w, --no-way-polygons        Do not output areas created from ways\n"
#ifdef WITH_OLD_STYLE_MP_SUPPORT
              << "  -x, --no-areas               Do not output areas (same as -s -S -w)\n"
#else
              << "  -x, --no-areas               Do not output areas (same as -s -w)\n"
#endif
              ;
}

class DummyAssembler {

    osmium::area::area_stats m_stats;

public:

    struct config_type {};

    explicit DummyAssembler(const config_type& /*config*/) {
    }

    void operator()(const osmium::Way& /*way*/, osmium::memory::Buffer& /*out_buffer*/) {
    }

    void operator()(const osmium::Relation& /*relation*/, const std::vector<const osmium::Way*>& /*members*/, osmium::memory::Buffer& /*out_buffer*/) {
    }

    const osmium::area::area_stats& stats() const noexcept {
        return m_stats;
    }

}; // class DummyAssembler

#ifdef WITH_OLD_STYLE_MP_SUPPORT
using assembler_type = osmium::area::AssemblerLegacy;
using mp_manager_type = osmium::area::MultipolygonManagerLegacy<assembler_type>;
using mp_manager_only = osmium::area::MultipolygonManagerLegacy<DummyAssembler>;
#else
using assembler_type = osmium::area::Assembler;
using mp_manager_type = osmium::area::MultipolygonManager<assembler_type>;
using mp_manager_only = osmium::area::MultipolygonManager<DummyAssembler>;
#endif

template <typename TMPManager>
void show_incomplete_relations(TMPManager& manager) {
    std::vector<osmium::object_id_type> incomplete_relation_ids;
    manager.for_each_incomplete_relation([&](const osmium::relations::RelationHandle& handle){
        incomplete_relation_ids.push_back(handle->id());
    });
    if (!incomplete_relation_ids.empty()) {
        std::cerr << "Warning! Some member ways missing for these multipolygon relations:";
        for (const auto id : incomplete_relation_ids) {
            std::cerr << " " << id;
        }
        std::cerr << '\n';
    }
}

class optional_output {

    std::unique_ptr<std::ostream> m_stream{};
    bool m_enabled = false;

public:

    void set_file(const char* filename) {
        m_enabled = true;
        m_stream = std::make_unique<std::ofstream>(filename);
    }

    void set_stdout() noexcept {
        m_enabled = true;
    }

    explicit operator bool() const noexcept {
        return m_enabled;
    }

    std::ostream& get() const noexcept {
        if (m_stream) {
            return *m_stream;
        }
        return std::cout;
    }

}; // class optional_output

int main(int argc, char* argv[]) {
    try {
        osmium::util::VerboseOutput vout{true};

        static const struct option long_options[] = {
            {"suppress-area-output", no_argument,       nullptr, 'a'},
            {"check",                no_argument,       nullptr, 'c'},
            {"collect-only",         no_argument,       nullptr, 'C'},
            {"only-invalid",         no_argument,       nullptr, 'f'},
            {"debug",                optional_argument, nullptr, 'd'},
            {"dump-areas",           optional_argument, nullptr, 'D'},
            {"empty-areas",          no_argument,       nullptr, 'e'},
            {"help",                 no_argument,       nullptr, 'h'},
            {"index",                required_argument, nullptr, 'i'},
            {"show-index",           no_argument,       nullptr, 'I'},
            {"output",               required_argument, nullptr, 'o'},
            {"overwrite",            no_argument,       nullptr, 'O'},
            {"report-problems",      optional_argument, nullptr, 'p'},
            {"show-incomplete",      no_argument,       nullptr, 'r'},
            {"check-roles",          no_argument,       nullptr, 'R'},
            {"no-new-style",         no_argument,       nullptr, 's'},
            {"no-old-style",         no_argument,       nullptr, 'S'},
            {"keep-type-tag",        no_argument,       nullptr, 't'},
            {"no-way-polygons",      no_argument,       nullptr, 'w'},
            {"no-areas",             no_argument,       nullptr, 'x'},
            {nullptr, 0, nullptr, 0}
        };

        std::string database_name;

        std::string location_index_type{"flex_mem"};
        const auto& map_factory = osmium::index::MapFactory<osmium::unsigned_object_id_type, osmium::Location>::instance();

        optional_output dump_stream;
        optional_output problem_stream;

        bool check = false;
        bool collect_only = false;
        bool only_invalid = false;
        bool show_incomplete = false;
        bool overwrite = false;
        bool output_areas = true;

        assembler_type::config_type assembler_config;
        assembler_config.create_empty_areas = false;

        while (true) {
            const int c = getopt_long(argc, argv, "acCd::D::efhi:Io:Op::rRsStwx", long_options, nullptr);
            if (c == -1) {
                break;
            }

            switch (c) {
                case 'a':
                    output_areas = false;
                    break;
                case 'c':
                    check = true;
                    break;
                case 'C':
                    collect_only = true;
                    break;
                case 'd':
                    assembler_config.debug_level = optarg ? std::atoi(optarg) : 1;
                    break;
                case 'D':
                    if (optarg) {
                        dump_stream.set_file(optarg);
                    } else {
                        dump_stream.set_stdout();
                    }
                    break;
                case 'e':
                    assembler_config.create_empty_areas = true;
                    break;
                case 'f':
                    only_invalid = true;
                    check = true;
                    break;
                case 'h':
                    print_help();
                    return exit_code_ok;
                case 'i':
                    location_index_type = optarg;
                    break;
                case 'I':
                    show_index_types();
                    return exit_code_ok;
                case 'o':
                    database_name = optarg;
                    break;
                case 'O':
                    overwrite = true;
                    break;
                case 'p':
                    if (optarg) {
                        problem_stream.set_file(optarg);
                    } else {
                        problem_stream.set_stdout();
                    }
                    break;
                case 'r':
                    show_incomplete = true;
                    break;
                case 'R':
                    assembler_config.check_roles = true;
                    break;
                case 's':
                    assembler_config.create_new_style_polygons = false;
                    break;
                case 'S':
                    assembler_config.create_old_style_polygons = false;
                    break;
                case 't':
                    assembler_config.keep_type_tag = true;
                    break;
                case 'w':
                    assembler_config.create_way_polygons = false;
                    break;
                case 'x':
                    assembler_config.create_new_style_polygons = false;
                    assembler_config.create_old_style_polygons = false;
                    assembler_config.create_way_polygons = false;
                    break;
                default:
                    return exit_code_cmdline_error;
            }
        }

        const int remaining_args = argc - optind;
        if (remaining_args != 1) {
            std::cerr << "Usage: " << argv[0] << " [OPTIONS] OSMFILE\n";
            return exit_code_cmdline_error;
        }

        auto location_index = map_factory.create_map(location_index_type);
        location_handler_type location_handler{*location_index};
        location_handler.ignore_errors(); // XXX

        const osmium::io::File input_file{argv[optind]};

        const bool need_locations = location_index_type != "none";

        if (collect_only) {
            DummyAssembler::config_type config;
            mp_manager_only mp_manager{config};

            vout << "Starting first pass (reading relations)...\n";
            osmium::relations::read_relations(input_file, mp_manager);
            vout << "First pass done.\n";

            vout << "Memory:\n";
            osmium::relations::print_used_memory(vout, mp_manager.used_memory());

            vout << "Starting second pass (reading nodes and ways and assembling areas)...\n";
            osmium::io::Reader reader2{input_file, entity_bits(location_index_type)};
            if (need_locations) {
                osmium::apply(reader2, location_handler, mp_manager.handler());
            } else {
                osmium::apply(reader2, mp_manager.handler());
            }
            reader2.close();
            vout << "Second pass done\n";

            vout << "Memory:\n";
            osmium::relations::print_used_memory(vout, mp_manager.used_memory());

            vout << "Stats:" << mp_manager.stats() << '\n';
        } else {
            std::unique_ptr<osmium::area::ProblemReporter> reporter{nullptr};

            if (problem_stream) {
                reporter = std::make_unique<osmium::area::ProblemReporterStream>(problem_stream.get());
                assembler_config.problem_reporter = reporter.get();
            }

            if (database_name.empty()) {
                mp_manager_type mp_manager{assembler_config};

                vout << "Starting first pass (reading relations)...\n";
                osmium::relations::read_relations(input_file, mp_manager);
                vout << "First pass done.\n";

                vout << "Memory:\n";
                osmium::relations::print_used_memory(vout, mp_manager.used_memory());

                vout << "Starting second pass (reading nodes and ways and assembling areas)...\n";
                osmium::io::Reader reader2{input_file, entity_bits(location_index_type)};
                if (need_locations) {
                    osmium::apply(reader2, location_handler, mp_manager.handler([](osmium::memory::Buffer&& /*buffer*/) {}));
                } else {
                    osmium::apply(reader2, mp_manager.handler([](osmium::memory::Buffer&& /*buffer*/) {}));
                }
                reader2.close();
                vout << "Second pass done\n";

                vout << "Memory:\n";
                osmium::relations::print_used_memory(vout, mp_manager.used_memory());

                vout << "Stats:" << mp_manager.stats() << '\n';

                if (show_incomplete) {
                    show_incomplete_relations(mp_manager);
                }
            } else {
                if (overwrite) {
                    unlink(database_name.c_str());
                }

                CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", "OFF");
                osmium::geom::OGRFactory<> factory;

                gdalcpp::Dataset dataset{"SQLite", database_name, gdalcpp::SRS{factory.proj_string()}, { "SPATIALITE=TRUE", "INIT_WITH_EPSG=NO" }};
                dataset.enable_auto_transactions();

                dataset.exec("PRAGMA journal_mode = OFF;");

                OutputOGR output{dataset, factory};
                output.set_check(check);
                output.set_only_invalid(only_invalid);
                output.set_output_areas(output_areas);

                if (!problem_stream) {
                    reporter = std::make_unique<osmium::area::ProblemReporterOGR>(dataset);
                }
                assembler_config.problem_reporter = reporter.get();
                mp_manager_type mp_manager{assembler_config};

                vout << "Starting first pass (reading relations)...\n";
                osmium::relations::read_relations(input_file, mp_manager);
                vout << "First pass done.\n";

                vout << "Memory:\n";
                osmium::relations::print_used_memory(vout, mp_manager.used_memory());

                vout << "Starting second pass (reading nodes and ways and assembling areas)...\n";
                osmium::io::Reader reader2{input_file, entity_bits(location_index_type)};

                if (dump_stream) {
                    osmium::handler::Dump dump_handler{dump_stream.get()};
                    if (need_locations) {
                        osmium::apply(reader2, location_handler, mp_manager.handler([&output, &dump_handler](osmium::memory::Buffer&& buffer) {
                            osmium::apply(buffer, dump_handler, output);
                        }));
                    } else {
                        osmium::apply(reader2, mp_manager.handler([&output, &dump_handler](osmium::memory::Buffer&& buffer) {
                            osmium::apply(buffer, dump_handler, output);
                        }));
                    }
                } else {
                    if (need_locations) {
                        osmium::apply(reader2, location_handler, mp_manager.handler([&output](osmium::memory::Buffer&& buffer) {
                            osmium::apply(buffer, output);
                        }));
                    } else {
                        osmium::apply(reader2, mp_manager.handler([&output](osmium::memory::Buffer&& buffer) {
                            osmium::apply(buffer, output);
                        }));
                    }
                }

                reader2.close();
                vout << "Second pass done\n";

                if (!problem_stream) {
                    reporter.reset();
                }

                osmium::relations::print_used_memory(vout, mp_manager.used_memory());

                vout << "Stats:" << mp_manager.stats() << '\n';

                if (show_incomplete) {
                    show_incomplete_relations(mp_manager);
                }
            }
        }

        vout << "Estimated memory usage:\n";
        vout << "  location index: " << (location_index->used_memory() / 1024) << "kB\n";

        osmium::MemoryUsage mcheck;
        vout << "Actual memory usage:\n"
            << "  current: " << mcheck.current() << "MB\n"
            << "  peak:    " << mcheck.peak() << "MB\n";

        vout << "Done.\n";

    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return exit_code_error;
    }

    return exit_code_ok;
}

