/*
Copyright (C) 2016  The AlterPCB team
Contact: Maarten Baert <maarten-baert@hotmail.com>

This file is part of AlterPCB.

AlterPCB is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

AlterPCB is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this AlterPCB.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "CommandLine.h"

#include "ApplicationDirs.h"
#include "FindRoot.h"
#include "Json.h"
#include "MaterialDatabase.h"
#include "StringRegistry.h"
#include "TLineTypes.h"

#include <fstream>
#include <iostream>

static const char *const MESH_DETAIL_NAMES[] = {
	"very-low",
	"lower",
	"low",
	"medium",
	"high",
	"higher",
	"very-high",
};
constexpr size_t MESH_DETAIL_COUNT = sizeof(MESH_DETAIL_NAMES) / sizeof(MESH_DETAIL_NAMES[0]);
constexpr size_t MESH_DETAIL_MEDIUM = 3;

static const char *const USAGE =
	"Usage: alterpcb-tlinesim [OPTION]...\n"
	"Run without options to start the graphical user interface. With options, the simulator runs headlessly.\n"
	"\n"
	"Information:\n"
	"  -h, --help                      Show this help and exit.\n"
	"      --version                   Show the version and exit.\n"
	"      --list-types                List the available transmission line types.\n"
	"      --list-parameters           List the parameters and results of the type selected with --type.\n"
	"      --list-materials            List the available conductor and dielectric materials.\n"
	"\n"
	"Simulation setup:\n"
	"  -t, --type TYPE                 Transmission line type (index or name, see --list-types).\n"
	"  -p, --param NAME=VALUE          Set a parameter (repeatable). Lengths are in mm. Unset parameters\n"
	"                                  use their default value.\n"
	"  -f, --frequency GHZ             Simulation frequency in GHz (default: 1).\n"
	"  -m, --mesh-detail LEVEL         Mesh detail: very-low, lower, low, medium (default), high, higher,\n"
	"                                  very-high.\n"
	"      --materials FILE            Material database to use instead of the built-in materials.json.\n"
	"  -o, --output FILE               Write results to FILE instead of standard output.\n"
	"  -v, --verbose                   Show solver debug output on standard error.\n"
	"\n"
	"Simulation type (default: single frequency):\n"
	"      --frequency-sweep MIN:MAX:STEP\n"
	"                                  Sweep the frequency (in GHz).\n"
	"      --parameter-sweep NAME=MIN:MAX:STEP\n"
	"                                  Sweep a real-valued parameter at the frequency given by --frequency.\n"
	"      --tune NAME --target RESULT=VALUE\n"
	"                                  Find the value of a real-valued parameter that makes RESULT equal to\n"
	"                                  VALUE, e.g. --tune track-width --target impedance=50.\n"
	"\n"
	"Names of types, parameters and results are matched case-insensitively, ignoring punctuation\n"
	"(e.g. 'Track Width' and 'track-width' are equivalent). Results are named '<mode> <result>', e.g.\n"
	"'differential-impedance'. Without a mode prefix, the first mode is used.\n"
	"\n"
	"Results are written as tab-separated values with a header line. The first column is the frequency (Hz),\n"
	"the swept parameter, or the tuned parameter.\n"
	"\n"
	"Example:\n"
	"  alterpcb-tlinesim -t microstrip-single -p track-width=0.3 -p substrate-thickness=0.2 -f 2.4\n";

class CommandLineError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Discards everything written to std::cerr (the solver prints debug output there) while in scope.
class CerrSilencer {
private:
	std::streambuf *m_old_buffer;
public:
	CerrSilencer() : m_old_buffer(std::cerr.rdbuf(NULL)) {}
	~CerrSilencer() { std::cerr.rdbuf(m_old_buffer); std::cerr.clear(); }
};

// Normalizes a name for matching: lowercase alphanumeric words separated by single dashes.
static std::string Slug(const std::string &name) {
	std::string out;
	bool dash = false;
	for(char c : name) {
		if(c >= 'A' && c <= 'Z')
			c = (char) (c - 'A' + 'a');
		if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			if(dash && !out.empty())
				out += '-';
			dash = false;
			out += c;
		} else {
			dash = true;
		}
	}
	return out;
}

static std::pair<std::string, std::string> SplitAssignment(const std::string &str, const char *option) {
	size_t pos = str.find('=');
	if(pos == std::string::npos)
		throw CommandLineError(MakeString("Option ", option, " expects NAME=VALUE, got '", str, "'."));
	return {str.substr(0, pos), str.substr(pos + 1)};
}

static void ParseRange(const std::string &str, const char *option, real_t &min, real_t &max, real_t &step) {
	size_t pos1 = str.find(':');
	size_t pos2 = (pos1 == std::string::npos)? std::string::npos : str.find(':', pos1 + 1);
	if(pos2 == std::string::npos)
		throw CommandLineError(MakeString("Option ", option, " expects a range MIN:MAX:STEP, got '", str, "'."));
	min = FloatFromString(str.substr(0, pos1));
	max = FloatFromString(str.substr(pos1 + 1, pos2 - pos1 - 1));
	step = FloatFromString(str.substr(pos2 + 1));
	if(!FinitePositive(step))
		throw CommandLineError(MakeString("Option ", option, ": step must be positive."));
}

static size_t FindTLineType(const std::string &name) {
	bool numeric = !name.empty() && name.find_first_not_of("0123456789") == std::string::npos;
	if(numeric) {
		size_t index = (size_t) std::stoul(name);
		if(index < g_tline_types.size())
			return index;
	}
	for(size_t i = 0; i < g_tline_types.size(); ++i) {
		if(Slug(g_tline_types[i].m_name) == Slug(name))
			return i;
	}
	throw CommandLineError(MakeString("Unknown transmission line type '", name, "' (see --list-types)."));
}

static size_t FindParameter(const TLineType &tline_type, const std::string &name) {
	for(size_t i = 0; i < tline_type.m_parameters.size(); ++i) {
		if(Slug(tline_type.m_parameters[i].m_name) == Slug(name))
			return i;
	}
	throw CommandLineError(MakeString("Unknown parameter '", name, "' for type '", tline_type.m_name, "' (see --list-parameters)."));
}

static size_t FindRealParameter(const TLineType &tline_type, const std::string &name) {
	size_t index = FindParameter(tline_type, name);
	if(tline_type.m_parameters[index].m_type != TLINE_PARAMETERTYPE_REAL)
		throw CommandLineError(MakeString("Parameter '", tline_type.m_parameters[index].m_name, "' is not real-valued."));
	return index;
}

static size_t FindResult(const TLineType &tline_type, const std::string &name) {
	for(size_t i = 0; i < tline_type.m_modes.size(); ++i) {
		for(size_t j = 0; j < TLINERESULT_COUNT; ++j) {
			if(Slug(tline_type.m_modes[i] + " " + TLINERESULT_NAMES[j]) == Slug(name) || (i == 0 && Slug(TLINERESULT_NAMES[j]) == Slug(name)))
				return TLINERESULT_COUNT * i + j;
		}
	}
	throw CommandLineError(MakeString("Unknown result '", name, "' for type '", tline_type.m_name, "' (see --list-parameters)."));
}

static VData ParseParameterValue(const TLineParameter &parameter, const std::string &str, MaterialDatabase &material_database) {
	switch(parameter.m_type) {
		case TLINE_PARAMETERTYPE_BOOL: {
			std::string s = Slug(str);
			if(s == "true" || s == "1" || s == "yes" || s == "on")
				return true;
			if(s == "false" || s == "0" || s == "no" || s == "off")
				return false;
			throw CommandLineError(MakeString("Parameter '", parameter.m_name, "' expects true or false, got '", str, "'."));
		}
		case TLINE_PARAMETERTYPE_REAL: {
			VData value = Json::FromString(str);
			FloatFromVData(value); // validate
			return value;
		}
		case TLINE_PARAMETERTYPE_MATERIAL_CONDUCTOR: {
			for(const MaterialConductor &material : material_database.GetConductors()) {
				if(Slug(material.m_name) == Slug(str))
					return material.m_name;
			}
			throw CommandLineError(MakeString("Unknown conductor '", str, "' (see --list-materials)."));
		}
		case TLINE_PARAMETERTYPE_MATERIAL_DIELECTRIC: {
			for(const MaterialDielectric &material : material_database.GetDielectrics()) {
				if(Slug(material.m_name) == Slug(str))
					return material.m_name;
			}
			throw CommandLineError(MakeString("Unknown dielectric '", str, "' (see --list-materials)."));
		}
		case TLINE_PARAMETERTYPE_COUNT: {
			assert(false);
			break;
		}
	}
	return VData();
}

static const char* ParameterTypeName(TLineParameterType type) {
	switch(type) {
		case TLINE_PARAMETERTYPE_BOOL: return "bool";
		case TLINE_PARAMETERTYPE_REAL: return "real";
		case TLINE_PARAMETERTYPE_MATERIAL_CONDUCTOR: return "conductor";
		case TLINE_PARAMETERTYPE_MATERIAL_DIELECTRIC: return "dielectric";
		case TLINE_PARAMETERTYPE_COUNT: break;
	}
	return "?";
}

static void WriteHeader(std::ostream &out, const TLineType &tline_type, const std::string &first_column) {
	out << first_column;
	for(size_t i = 0; i < tline_type.m_modes.size(); ++i) {
		for(size_t j = 0; j < TLINERESULT_COUNT; ++j) {
			out << '\t' << tline_type.m_modes[i] << ' ' << TLINERESULT_NAMES[j];
		}
	}
	out << std::endl;
}

static void WriteRow(std::ostream &out, const TLineType &tline_type, real_t first_value, const real_t *results) {
	out << first_value;
	for(size_t j = 0; j < TLINERESULT_COUNT * tline_type.m_modes.size(); ++j) {
		out << '\t' << results[j];
	}
	out << std::endl;
}

enum SimulationType {
	SIMULATION_SINGLE_FREQUENCY,
	SIMULATION_FREQUENCY_SWEEP,
	SIMULATION_PARAMETER_SWEEP,
	SIMULATION_PARAMETER_TUNE,
};

struct CommandLineOptions {
	bool m_help = false, m_version = false, m_verbose = false;
	bool m_list_types = false, m_list_parameters = false, m_list_materials = false;
	std::string m_type;
	std::vector<std::pair<std::string, std::string>> m_parameters;
	std::string m_frequency = "1";
	size_t m_mesh_detail = MESH_DETAIL_MEDIUM;
	std::string m_materials_file, m_output_file;
	std::string m_frequency_sweep, m_parameter_sweep, m_tune, m_target;
};

static CommandLineOptions ParseOptions(int argc, char* argv[]) {
	CommandLineOptions options;
	for(int i = 1; i < argc; ++i) {
		std::string arg = argv[i], value;
		bool has_value = false;
		if(arg.compare(0, 2, "--") == 0) {
			size_t pos = arg.find('=');
			if(pos != std::string::npos) {
				value = arg.substr(pos + 1);
				arg = arg.substr(0, pos);
				has_value = true;
			}
		}
		auto next = [&]() -> const std::string& {
			if(!has_value) {
				if(i + 1 >= argc)
					throw CommandLineError(MakeString("Option ", arg, " requires a value."));
				value = argv[++i];
				has_value = true;
			}
			return value;
		};
		if(arg == "-h" || arg == "--help") {
			options.m_help = true;
		} else if(arg == "--version") {
			options.m_version = true;
		} else if(arg == "-v" || arg == "--verbose") {
			options.m_verbose = true;
		} else if(arg == "--list-types") {
			options.m_list_types = true;
		} else if(arg == "--list-parameters") {
			options.m_list_parameters = true;
		} else if(arg == "--list-materials") {
			options.m_list_materials = true;
		} else if(arg == "-t" || arg == "--type") {
			options.m_type = next();
		} else if(arg == "-p" || arg == "--param") {
			options.m_parameters.push_back(SplitAssignment(next(), "--param"));
		} else if(arg == "-f" || arg == "--frequency") {
			options.m_frequency = next();
		} else if(arg == "-m" || arg == "--mesh-detail") {
			std::string level = Slug(next());
			size_t j = 0;
			while(j < MESH_DETAIL_COUNT && level != MESH_DETAIL_NAMES[j])
				++j;
			if(j == MESH_DETAIL_COUNT)
				throw CommandLineError(MakeString("Unknown mesh detail level '", value, "'."));
			options.m_mesh_detail = j;
		} else if(arg == "--materials") {
			options.m_materials_file = next();
		} else if(arg == "-o" || arg == "--output") {
			options.m_output_file = next();
		} else if(arg == "--frequency-sweep") {
			options.m_frequency_sweep = next();
		} else if(arg == "--parameter-sweep") {
			options.m_parameter_sweep = next();
		} else if(arg == "--tune") {
			options.m_tune = next();
		} else if(arg == "--target") {
			options.m_target = next();
		} else {
			throw CommandLineError(MakeString("Unknown option '", argv[i], "' (see --help)."));
		}
	}
	return options;
}

static void LoadMaterials(MaterialDatabase &material_database, const std::string &filename) {
	std::string file = filename;
	if(file.empty()) {
		if(g_application_data_dir.isEmpty())
			throw std::runtime_error("Could not load materials, data directory is missing. Use --materials to specify a file.");
		file = g_application_data_dir.toStdString() + "/materials.json";
	}
	try {
		material_database.LoadFile(file);
		material_database.Finish();
	}
	catch(const std::runtime_error &e) {
		throw std::runtime_error(MakeString("Could not load material database '", file, "': ", e.what()));
	}
}

static void ListTypes(std::ostream &out) {
	for(size_t i = 0; i < g_tline_types.size(); ++i) {
		out << i << '\t' << Slug(g_tline_types[i].m_name) << '\t' << g_tline_types[i].m_name << std::endl;
	}
}

static void ListParameters(std::ostream &out, const TLineType &tline_type) {
	out << tline_type.m_name << std::endl << tline_type.m_description << std::endl << std::endl;
	out << "Parameters:" << std::endl;
	for(const TLineParameter &parameter : tline_type.m_parameters) {
		out << "  " << Slug(parameter.m_name) << '\t' << ParameterTypeName(parameter.m_type)
			<< "\tdefault: " << ((parameter.m_type == TLINE_PARAMETERTYPE_REAL)? Json::ToString(parameter.m_default_value) :
								  (parameter.m_type == TLINE_PARAMETERTYPE_BOOL)? (parameter.m_default_value.AsBool()? "true" : "false") :
																				  parameter.m_default_value.AsString())
			<< ((parameter.m_unit_mm)? " mm" : "") << std::endl;
	}
	out << std::endl << "Results:" << std::endl;
	for(size_t i = 0; i < tline_type.m_modes.size(); ++i) {
		for(size_t j = 0; j < TLINERESULT_COUNT; ++j) {
			out << "  " << Slug(tline_type.m_modes[i] + " " + TLINERESULT_NAMES[j]) << '\t' << TLINERESULT_UNITS[j] << std::endl;
		}
	}
}

static void ListMaterials(std::ostream &out, MaterialDatabase &material_database) {
	out << "Conductors:" << std::endl;
	for(const MaterialConductor &material : material_database.GetConductors()) {
		out << "  " << material.m_name << std::endl;
	}
	out << std::endl << "Dielectrics:" << std::endl;
	for(const MaterialDielectric &material : material_database.GetDielectrics()) {
		out << "  " << material.m_name << std::endl;
	}
}

static void Simulate(const CommandLineOptions &options, const TLineType &tline_type, MaterialDatabase &material_database, std::ostream &out) {

	// determine simulation type
	SimulationType simulation_type = SIMULATION_SINGLE_FREQUENCY;
	size_t num_types = 0;
	if(!options.m_frequency_sweep.empty()) {
		simulation_type = SIMULATION_FREQUENCY_SWEEP;
		++num_types;
	}
	if(!options.m_parameter_sweep.empty()) {
		simulation_type = SIMULATION_PARAMETER_SWEEP;
		++num_types;
	}
	if(!options.m_tune.empty()) {
		simulation_type = SIMULATION_PARAMETER_TUNE;
		++num_types;
	}
	if(num_types > 1)
		throw CommandLineError("Options --frequency-sweep, --parameter-sweep and --tune are mutually exclusive.");
	if((simulation_type == SIMULATION_PARAMETER_TUNE) != !options.m_target.empty())
		throw CommandLineError("Options --tune and --target must be used together.");

	// initialize context
	TLineContext context;
	context.m_material_database = &material_database;
	context.m_mesh_detail = exp2((real_t) ((int) options.m_mesh_detail - (int) MESH_DETAIL_MEDIUM) * 0.5);
	std::vector<VData> values(tline_type.m_parameters.size());
	for(size_t i = 0; i < tline_type.m_parameters.size(); ++i) {
		values[i] = tline_type.m_parameters[i].m_default_value;
	}
	for(const auto &assignment : options.m_parameters) {
		size_t index = FindParameter(tline_type, assignment.first);
		values[index] = ParseParameterValue(tline_type.m_parameters[index], assignment.second, material_database);
	}
	for(size_t i = 0; i < tline_type.m_parameters.size(); ++i) {
		stringtag_t key = StringRegistry::NewTag(CanonicalName(tline_type.m_parameters[i].m_name));
		context.m_parameters.EmplaceBack(VDataDictEntry(key, std::move(values[i])));
	}
	context.m_frequencies = {FloatFromString(options.m_frequency) * 1e9};

	size_t result_size = TLINERESULT_COUNT * tline_type.m_modes.size();
	switch(simulation_type) {
		case SIMULATION_SINGLE_FREQUENCY: {
			tline_type.m_simulate(context);
			WriteHeader(out, tline_type, "Frequency");
			WriteRow(out, tline_type, context.m_frequencies[0], context.m_results.data());
			break;
		}
		case SIMULATION_FREQUENCY_SWEEP: {
			real_t freq_min, freq_max, freq_step;
			ParseRange(options.m_frequency_sweep, "--frequency-sweep", freq_min, freq_max, freq_step);
			MakeSweep(context.m_frequencies, freq_min * 1e9, freq_max * 1e9, freq_step * 1e9);
			tline_type.m_simulate(context);
			WriteHeader(out, tline_type, "Frequency");
			for(size_t i = 0; i < context.m_frequencies.size(); ++i) {
				WriteRow(out, tline_type, context.m_frequencies[i], context.m_results.data() + result_size * i);
			}
			break;
		}
		case SIMULATION_PARAMETER_SWEEP: {
			auto assignment = SplitAssignment(options.m_parameter_sweep, "--parameter-sweep");
			size_t param_index = FindRealParameter(tline_type, assignment.first);
			real_t value_min, value_max, value_step;
			ParseRange(assignment.second, "--parameter-sweep", value_min, value_max, value_step);
			std::vector<real_t> sweep_values;
			MakeSweep(sweep_values, value_min, value_max, value_step);
			WriteHeader(out, tline_type, tline_type.m_parameters[param_index].m_name);
			for(size_t i = 0; i < sweep_values.size(); ++i) {
				context.m_parameters[param_index].Value() = FloatScale(sweep_values[i]);
				tline_type.m_simulate(context);
				WriteRow(out, tline_type, sweep_values[i], context.m_results.data());
			}
			break;
		}
		case SIMULATION_PARAMETER_TUNE: {
			size_t param_index = FindRealParameter(tline_type, options.m_tune);
			auto assignment = SplitAssignment(options.m_target, "--target");
			size_t result_index = FindResult(tline_type, assignment.first);
			real_t target_value = FloatFromString(assignment.second);
			real_t initial_value = FloatFromVData(context.m_parameters[param_index].Value());
			if(!FinitePositive(initial_value)) {
				initial_value = FloatFromVData(tline_type.m_parameters[param_index].m_default_value);
			}
			real_t root_value = FindRootRelative([&context, &tline_type, param_index, result_index, target_value](real_t x) {
				context.m_parameters[param_index].Value() = FloatScale(x);
				tline_type.m_simulate(context);
				return context.m_results[result_index] - target_value;
			}, initial_value, 1e-8, target_value * 1e-8, 1e6);
			WriteHeader(out, tline_type, tline_type.m_parameters[param_index].m_name);
			WriteRow(out, tline_type, root_value, context.m_results.data());
			break;
		}
	}

}

int RunCommandLine(int argc, char* argv[]) {
	try {

		CommandLineOptions options = ParseOptions(argc, argv);
		if(options.m_help) {
			std::cout << USAGE;
			return 0;
		}
		if(options.m_version) {
			std::cout << "alterpcb-tlinesim " << ALTERPCB_VERSION << std::endl;
			return 0;
		}
		if(options.m_list_types) {
			ListTypes(std::cout);
			return 0;
		}

		MaterialDatabase material_database;
		LoadMaterials(material_database, options.m_materials_file);
		if(options.m_list_materials) {
			ListMaterials(std::cout, material_database);
			return 0;
		}

		if(options.m_type.empty())
			throw CommandLineError("No transmission line type specified (use --type, see --list-types).");
		const TLineType &tline_type = g_tline_types[FindTLineType(options.m_type)];
		if(options.m_list_parameters) {
			ListParameters(std::cout, tline_type);
			return 0;
		}

		std::ofstream file;
		if(!options.m_output_file.empty()) {
			file.open(options.m_output_file, std::ios_base::out | std::ios_base::trunc);
			if(file.fail())
				throw std::runtime_error("Could not open file '" + options.m_output_file + "' for writing.");
		}
		std::unique_ptr<CerrSilencer> silencer;
		if(!options.m_verbose)
			silencer.reset(new CerrSilencer());
		Simulate(options, tline_type, material_database, (options.m_output_file.empty())? std::cout : file);

	}
	catch(const CommandLineError &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 2;
	}
	catch(const std::exception &e) {
		std::cerr << "Simulation failed: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
