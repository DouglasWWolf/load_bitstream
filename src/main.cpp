#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdarg>
#include <iostream>
#include "config_file.h"
#include "PciDevice.h"

// Bring in the std library
using namespace std;


// Global variables
bool      performHotReset = false;
string    configFile      = "load_bitstream.conf";
string    bitstream;
PciDevice PCI;

// These values are read in from the config file durint init()
struct
{
    string          tmpDir;
    string          vivado;
    string          pciDevice;
    vector<string>  programmingScript;
} config;

//=================================================================================================
//                                Forward Declarations
//=================================================================================================
void execute();
void parseCommandLine(int argc, const char** argv);
void readConfigFile(string filename);
void loadBitstream();
void hotReset(string device);
void performMacroSubstitutions(vector<string>& v);
//=================================================================================================


//=================================================================================================
// main() - Program execution begins here
//=================================================================================================
int main(int argc, const char** argv)
{
    // Parse the command line
    parseCommandLine(argc, argv);

    // Execute the main body of the program
    try
    {
        execute();
    }

    // If anything throws a runtime error, display it and exit
    catch(const std::runtime_error& e)
    {
        std::cerr << e.what() << '\n';
        exit(1);
    }

    // If we get here, all is well    
    return 0;
}
//=================================================================================================


//=================================================================================================
// execute() - This is the main body of the program
//=================================================================================================
void execute()
{
    // If we're not running with root privileges, give up
    if (geteuid() != 0) throw runtime_error("Must be root to run.  Use sudo.");

    // Read the configuration file
    readConfigFile(configFile);

    // Perform macro substitutions on the programming script
    performMacroSubstitutions(config.programmingScript);

    // Load the bitstream into the FPGA
    loadBitstream();

    // If the user requested a hot-reset, re-enumerate the PCI bus
    if (performHotReset) PCI.hotReset(config.pciDevice);
}
//=================================================================================================





//==========================================================================================================
// throwRuntime() - Throws a runtime exception
//==========================================================================================================
static void throwRuntime(const char* fmt, ...)
{
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buffer, fmt, ap);
    va_end(ap);

    throw runtime_error(buffer);
}
//=========================================================================================================


//==========================================================================================================
// c() - Shorthand way of converting a std::string to a const char*
//==========================================================================================================
const char* c(string& s) {return s.c_str();}
//==========================================================================================================


//==========================================================================================================
// chomp() - Removes any carriage-return or linefeed from the end of a buffer
//==========================================================================================================
static void chomp(char* buffer)
{
    char* p;
    p = strchr(buffer, 10);
    if (p) *p = 0;
    p = strchr(buffer, 13);
    if (p) *p = 0;
}
//==========================================================================================================


//=================================================================================================
// shell() - Executes a shell command and returns the output as a vector of strings
//=================================================================================================
static vector<string> shell(const char* fmt, ...)
{
    vector<string> result;
    va_list        ap;
    char           command[1024];
    char           buffer[1024];

    // Format the command
    va_start(ap, fmt);
    vsprintf(command, fmt, ap);
    va_end(ap);

    // Run the command
    FILE* fp = popen(command, "r");

    // If we couldn't do that, give up
    if (fp == nullptr) return result;

    // Fetch every line of the output and push it to our vector of strings
    while (fgets(buffer, sizeof buffer, fp))
    {
        chomp(buffer);
        result.push_back(buffer);
    }

    // When the program finishes, close the FILE*
    fclose(fp);

    // And hand the output of the program (1 string per line) to the caller
    return result;
}
//=================================================================================================



//=================================================================================================
// writeStrVecToFile() - Helper function that writes a vector of strings to a file, with a linefeed 
//                       appended to the end of each line.
//=================================================================================================
static bool writeStrVecToFile(vector<string>& v, string filename)
{
    // Create the output file
    FILE* ofile = fopen(c(filename), "w");
    
    // If we can't create the output file, whine to the caller
    if (ofile == nullptr) return false;    

    // Write each line in the vector to the output file
    for (string& s : v) fprintf(ofile, "%s\n", c(s));

    // We're done with the output file
    fclose(ofile);

    // And tell the caller that all is well
    return true;
}
//==========================================================================================================


//=================================================================================================
// parseCommandLine() - Parses the command line
//
// On Exit: bitstream       = Name of the bitstream file
//          performHotReset = true, if we should do a PCI hot_reset after loading bitstream
//          configFile      = Name of the configuration file
//=================================================================================================
void parseCommandLine(int argc, const char** argv)
{
    int idx = 1;

    // So long as we have parameters to parse...
    while (idx < argc)
    {
        // Fetch the next command-line argument
        string arg = argv[idx++];
        
        // Is this the "-hot_reset" switch?
        if (arg ==  "-hot_reset")
            performHotReset = true;

        // Is the user specifying a config file?
        else if (arg == "-config" && argv[idx])
            configFile = argv[idx++];
        
        // Is the user specifying the filename of the bitstream?
        else if (arg[0] != '-' && bitstream.empty())
            bitstream = arg;

        // Otherwise, complain about the invalid switch
        else
        {
            printf("invalid command-line switch: %s\n", c(arg));
            exit(1);
        }
    }

    // If there's no filename on the command line, just show the usage
    if (bitstream.empty())
    {
        printf("usage:\n");
        printf("load_bitstream <filename> [-hot_reset] [-config <filename>]\n");
        exit(1);
    }
}
//=================================================================================================



//=================================================================================================
// readConfigFile() - Reads in the configuration file
//=================================================================================================
void readConfigFile(string filename)
{
    CConfigFile cf;
    CConfigScript cs;

    // Read the configuration file and complain if we can't.
    if (!cf.read(filename, false)) throw runtime_error("Cant read file "+filename);

    // Fetch the name of the temporary directory
    cf.get("tmp_dir", &config.tmpDir);

    // Fetch the name of the Vivado executable
    cf.get("vivado", &config.vivado);

    // Fetch the PCI vendorID:deviceID of the FPGA card
    if (performHotReset) cf.get("pci_device", &config.pciDevice);

    // Fetch the TCL script that we will use to program the bitstream
    cf.get_script_vector("programming_script", &config.programmingScript);

}
//=================================================================================================


//=================================================================================================
// replace() - Replaces the "from" string with the "to" string if the "from" string exists
//=================================================================================================
void replace(string& str, string from, string to)
{
    size_t start_pos = str.find(from);
    if (start_pos == std::string::npos) return;
    str.replace(start_pos, from.length(), to);
}
//=================================================================================================


//=================================================================================================
// performMacroSubstitutions() - Performs macros substitutions in a vector of strings
//
// A "macro" is any string in the form "%some_keyword%"
//=================================================================================================
void performMacroSubstitutions(vector<string>& v)
{
    for (auto& line : v)
    {
        replace(line, "\%file\%", bitstream);
    }
}
//=================================================================================================



//=================================================================================================
// loadBitstream - Programs the bitstream into the FPGA
//=================================================================================================
void loadBitstream()
{
    // Create the filename of the TCL script we want Vivado to execute
    string tclFilename = config.tmpDir + "/load_bitstream.tcl";
    
    // Create the filename where we want to store the script output
    string resultFilename = config.tmpDir + "/load_bitstream.result";

    // Write the master-bitstream TCL script to disk
    if (!writeStrVecToFile(config.programmingScript, tclFilename)) 
    {
        throwRuntime("Can't write %s", c(tclFilename));
    }

    // Use Vivado to load the bitstream into the FPGA via JTAG
    vector<string> result = shell("%s 2>&1 -nojournal -nolog -mode batch -source %s", c(config.vivado), c(tclFilename));

    // If there was no output from that, it means we couldn't find Vivado
    if (result.size() < 3)
    {
        throwRuntime("Can't run %s", c(config.vivado));
    }

    // Write the Vivado output to a file for later inspection
    writeStrVecToFile(result, resultFilename);

    // Loop through each line of the Vivado output
    for (auto& s : result)
    {
        // Extract the first word from the line
        std::string firstWord = s.substr(0, s.find(" "));     

        // If the first word is "ERROR:", report the failure
        if (firstWord == "ERROR:") throwRuntime("Vivado reports '%s'", c(s));
    }
}
//=================================================================================================


