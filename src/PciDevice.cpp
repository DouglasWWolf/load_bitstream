//=================================================================================================
// PciDevice.cpp - Implements a generic class for mapping PCIe devices into user-space
//=================================================================================================
#include <unistd.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "PciDevice.h"
using namespace std;

#define c(s) s.c_str()

// A convenient shortcut to std::filesystem
namespace fs = std::filesystem;

//=================================================================================================
// FileDes - This is a standard Unix/Linux file descriptor that closes itself
//           when it goes out of scope
//=================================================================================================
class FileDes
{
public:
    // Constructor with no value
    FileDes() {fd = -1;}

    // Constructor with a file descriptor
    FileDes(int value) {fd = value;}

    // No copy or assignment constructor - objects of this class can't be copied
    FileDes (const FileDes&) = delete;
    FileDes& operator= (const FileDes&) = delete;

    // Destructor - Closes the file descriptor
    ~FileDes() {if (fd != -1) ::close(fd);}

    // Assignment from an int
    FileDes& operator=(int value) {fd=value; return *this;} 

    // Conversion to an int
    operator int() {return fd;}

    // The actual file descriptor
    int fd;
};
//=================================================================================================




//=================================================================================================
// throwRuntime() - Throws a runtime exception
//=================================================================================================
static void throwRuntime(const char* fmt, ...)
{
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buffer, fmt, ap);
    va_end(ap);

    throw runtime_error(buffer);
}
//=================================================================================================


//=================================================================================================
// run() - Runs a shell command and returns it's output as a vector of lines
//=================================================================================================
static vector<string> run(const char* fmt, ...)
{
    char command[1024];
    char buffer[1024];
    vector<string> result;

    // Format the caller's command into 'cmd'
    va_list ap;
    va_start(ap, fmt);
    vsprintf(command, fmt, ap);
    va_end(ap);

    // Open the lpsci command as a process
    FILE* fp = popen(command, "r");

    // If we couldn't do that, give up
    if (fp == nullptr) throwRuntime("Can't run '%s'", command);

    // Fetch each line of the output
    while (fgets(buffer, sizeof buffer, fp))
    {
        // Chop the linefeed off the end
        char* p = strchr(buffer, 10); if (p) *p = 0;

        // Append this line to our result
        result.push_back(buffer);

    }

    // We're done with the program, close it's stdout
    fclose(fp);

    // Return a vector of string (one per line of output) to the caller
    return result;
}
//=================================================================================================


//=================================================================================================
// getBDF() - Converts a <vendorID:deviceID> into a PCI BDF
//=================================================================================================
static string getBDF(string device)
{
    char command[64];
    char buffer[256];
    string retval = "0000:";

    // Build the command "lspci -d {device}"
    sprintf(command, "lspci -d %s", device.c_str());

    // Open the lpsci command as a process
    FILE* fp = popen(command, "r");

    // If we couldn't do that, give up
    if (fp == nullptr) return "";

    // Fetch the first line of the output
    if (!fgets(buffer, sizeof buffer, fp))
    {
        fclose(fp);
        return "";
    }

    // Find the first space in the line
    char* p = strchr(buffer, ' ');
    if (p == nullptr)
    {
        fclose(fp);
        return "";
    }

    // Terminate the string at the first space. 
    *p = 0;
    
    // The PCI BDF of the device is the first token in the buffer
    return retval + buffer;
}
//=================================================================================================




//=================================================================================================
// mapResources() - Maps each memory-mappable resource for this device into user-space
//
// On Entry: resource_ = list of memory-mappable resources (the phys addr and the size)
//
// On Exit:  resource_ = each entry has userspace "baseAddr" filled in
//=================================================================================================
void PciDevice::mapResources()
{
    const char* filename = "/dev/mem";

    // These are the memory protection flags we'll use when mapping the device into memory
    const int protection = PROT_READ | PROT_WRITE;

    // Open the /dev/mem device
    FileDes fd = ::open(filename, O_RDWR| O_SYNC);

    // If that open failed, we're done here
    if (fd < 0)
    {
        close();
        throwRuntime("Can't open %s", filename);
    }

    // Loop through each entry in the list of memory-mappable resources for this PCI device
    for (auto& bar : resource_)
    {
        // Map the resources of this PCI device's BAR into our user-space memory map
        void* ptr = ::mmap(0, bar.size, protection, MAP_SHARED, fd, bar.physAddr);

        // If a mapping error occurs, don't continue trying to map resources
        if (ptr == MAP_FAILED) 
        {
            close();
            throwRuntime("mmap failed on 0x%lx for size 0x%lx", bar.physAddr, bar.size);
        }
        
        // Otherwise, save the user-space address that our PCI resource is mapped to
        bar.baseAddr = (uint8_t*)ptr;
    }
}
//=================================================================================================


//=================================================================================================
// getIntegerFromFile() - Opens the specified file, reads the first line, expects to find an 
//                        integer encoded as an ASCII string, and returns the value of that string
//=================================================================================================
static int getIntegerFromFile(string filename)
{
    string line;
   
    // Open the specified file.  It will contain a line of ASCII data
    ifstream file(filename);

    // If we couldn't open the file, hand the caller an invalid value   
    if (!file.is_open()) return -1;
    
    // Fetch the first line of the file
    getline(file, line);

    // And hand the caller that line, decoded as an integer
    return stoi(line, 0, 0);
}
//=================================================================================================


//=================================================================================================
// close() - Unmap any memory mapped resources from this PCI device
//=================================================================================================
void PciDevice::close()
{
    // Loop through each resource slot, and if it's memory mapped, unmap it
    for (auto& resource : resource_)
    {
        if (resource.baseAddr) munmap(resource.baseAddr, resource.size); 
    }

    // Delete the list of memory-mapped resources
    resource_.clear();
}
//=================================================================================================



//=================================================================================================
// getResourceList() - Returns a vector of resource_t entries that describe each memory-mappable
//                     resource (i.e., BAR) that this PCI device supports
//
// On Entry: deviceDir = the name of the device directory that contains the "resource" file
//
//
// Notes: The resource file will contain 1 line of ASCII data for each potential mappable resource.
//        Each line contains 3 fields separated one space character:
//           (1) The physical starting address of the memory mapped resource
//           (2) The physical ending address of the memory mapped resource
//           (3) A set of flags that we don't care about    
//=================================================================================================
std::vector<PciDevice::resource_t> PciDevice::getResourceList(std::string deviceDir)
{
    string             line;
    vector<resource_t> result;
    
    // This file will contain 1 line per potential resource
    string filename = deviceDir + "/resource";

    // Open the specified file  
    ifstream file(filename);

    // If we couldn't open the file, hand the caller an invalid value   
    if (!file.is_open()) throwRuntime("Can't open %s", c(filename));
    
    // Loop through each line of the file...
    while (getline(file, line))
    {
        // Get pointers to the 1st and 2nd text fields of that line
        const char* p1 = c(line);
        const char* p2 = strchr(p1, ' ');
        
        // Parse the physical starting and ending address of this memory-mappable resource
        off_t starting_address = strtoll(p1, 0, 0);
        off_t ending_address   = strtoll(p2, 0, 0);

        // A starting address of 0 means "this line doesn't define a memory-mappable resource"
        if (starting_address == 0) continue;

        // Compute how many bytes long that memory region is
        size_t size = ending_address - starting_address + 1;

        // Append the description of this mappable resource into our result vector        
        result.push_back({0, size, starting_address});
    }

    // If there are no memory-mappable resources, create an error message
    if (result.empty()) throwRuntime("Device contains no memory-mappable resources");

    // Hand the caller the list of resources that can be memory mapped for this PCI device
    return result;
}
//=================================================================================================




//=================================================================================================
// open() - Opens a connection to the specified PCIe device
//
// Passed: deviceStr = The vendorID:deviceID of the PCIe device we're looking for
//         deviceDir = Name of the file-system directory where PCI device information can
//                     be found.   If empty-string, a sensible default is used
//=================================================================================================
void PciDevice::open(string deviceStr, string deviceDir)
{
    string  dirName;

    // We have not yet found the device that we're looking for
    bool found = false;

    // Get a const char* to the name of the device
    const char* device = deviceStr.c_str();    

    // Extract the Vendor ID from the device string
    int vendorID = strtoul(device, nullptr, 16);

    // Extract the Device ID from the device string
    const char* p = strchr(device, ':');
    if (p == nullptr) throwRuntime("Malformed device ID %s", device);
    int deviceID = strtoul(p+1, nullptr, 16);

    // If we already have a PCIe device mapped, unmap it
    close();

    // If the caller didn't specify a device-directory, use the default
    if (deviceDir.empty()) deviceDir = "/sys/bus/pci/devices";

    // Loop through the entry for each device in the specified directory...
    for (auto const& entry : fs::directory_iterator(deviceDir)) 
    {
        // Ignore any directory entry that isn't itself a directory
        if (!entry.is_directory()) continue;

        // Fetch the name of the directory that we're about to examine
        dirName = entry.path().string();

        // Fetch the vendor ID and device ID of this device
        int thisVendorID = getIntegerFromFile(dirName + "/vendor");
        int thisDeviceID = getIntegerFromFile(dirName + "/device");

        // If this vendor ID and device ID match the caller's, we have found 
        // the droid we're looking for.
        if (thisVendorID == vendorID && thisDeviceID == deviceID)
        {
            found = true;
            break;
        }
    }

    // If we couldn't find a device with that vendor ID and device ID, complain
    if (!found) throwRuntime("No PCI device found for vendor=0x%X, device=0x%X", vendorID, deviceID);

    // Fetch the physical address and size of each resource (i.e. BAR) that our device supports
    resource_ = getResourceList(dirName);

    // Memory map each of the PCI device resources into userspace
    mapResources();
}
//=================================================================================================


//=================================================================================================
// getPortFromBdf() - Returns the port (i.e, the PCI bridge) that the specied device is 
//                    attached to.
//=================================================================================================
static string getPortFromBdf(string bdf)
{
    // Construct the file name of the psuedo-file we are interested in
    string pfs = "/sys/bus/pci/devices/" + bdf;

    // Turn the name of that psuedo-file into a filesystem::path object
    fs::path pf(pfs);
   
    // Make sure the psuedo-file actualy exists
    if (!filesystem::exists(pf)) throwRuntime("Can't find %s", c(pfs));

    // Make sure that path is a symbolic link
    if (!fs::is_symlink(pf)) throwRuntime("Not a symlink: %s", c(pfs));

    // Find the target of that symbolic link
    fs::path target = fs::read_symlink(pf);

    // The filename at the end is our BDF.  Throw it away
    target.remove_filename();

    // Fetch the target as a string so we can strip the '/' off the end of it.
    string s = target.string();

    // Remove the '/' at the end of the path
    target = s.substr(0, s.length() -1 );

    // The filename at the end is the name of the port that our device is attached to
    string port = target.filename();

    // Return the name of the port to the caller
    return port;
}
//=================================================================================================


//=================================================================================================
// writeDeviceFile() - Writes the specified string to the specified psuedo-file
//=================================================================================================
static void writeDeviceFile(string filename, const char* s)
{
    // Find out how long the string is that we're writing
    int length = strlen(s);

    // Open the file
    int fd = open(c(filename), O_WRONLY);
    
    // If we can't complain
    if (fd < 0) throwRuntime("Can't open %s", c(filename));

    // Write the string to the device psuedo-file
    if (write(fd, s, length) < length) throwRuntime("Can't write to %s", c(filename));

    // We're done
    close(fd);
}
//=================================================================================================


//=================================================================================================
// hotReset() - Performs a PCI hot-reset of the specified device
//
// Passed: device = vendorID:deviceID
//
// Can throw std::runtime_error
//=================================================================================================
void PciDevice::hotReset(string device)
{
    // Force a rescan for PCI-bus endpoints
    writeDeviceFile("/sys/bus/pci/rescan", "1\n");

    // Find the BDF that corresponds to this device
    string bdf = getBDF(device);

    // If we didn't find the PCI device we are looking for, complain
    if (bdf.empty()) throwRuntime("Can't locate device %s", c(device));

    // Find out which PCI bridge this device is attached to
    string port = getPortFromBdf(bdf);

    // Construct the name of the device file that manipulates that port
    string pdf = "/sys/bus/pci/devices/" + port;

    // Make sure the port device file actually exists
    if (!fs::exists(pdf)) throwRuntime("Can't find %s", c(pdf));

    // Remove our device from its bridge
    writeDeviceFile("/sys/bus/pci/devices/"+bdf+"/remove", "1\n");

    // Perform the PCI hot-reset
    run("setpci -s %s BRIDGE_CONTROL=40:40", c(port));
    usleep(500000);
    run("setpci -s %s BRIDGE_CONTROL=00:40", c(port));
    usleep(500000);

    // Determine the name of the psuedo-file that is used to rescan our PCI bridge
    string pf = "/sys/bus/pci/devices/" + port + "/dev_rescan";
    if (!filesystem::exists(pf)) pf = "/sys/bus/pci/devices/" + port + "/rescan";

    // Rescan our PCI bridge for endpoints
    writeDeviceFile(pf, "1\n");

    // Enable bus-mastering from this PCI device
    run("setpci -s %s COMMAND=0106", c(bdf));
}
//=================================================================================================


