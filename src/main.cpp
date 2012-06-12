/*
 *  This file is part of the Xen Crashdump Analyser.
 *
 *  The Xen Crashdump Analyser is free software: you can redistribute
 *  it and/or modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation, either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  The Xen Crashdump Analyser is distributed in the hope that it will
 *  be useful, but WITHOUT ANY WARRANTY; without even the implied
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the Xen Crashdump Analyser.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 *  Copyright (c) 2011,2012 Citrix Inc.
 */

#include "util/log.hpp"
#include "util/macros.hpp"
#include "host.hpp"
#include "memory.hpp"
#include "abstract/elf.hpp"

#include <getopt.h>

#include <err.h>
#include <sysexits.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <inttypes.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/**
 * @file src/main.cpp
 * @author Andrew Cooper
 */

/// Version string
static const char * version_str = "2.1.0";

// Global variables
int verbosity = 1;

// Local variables

/// Command line short options.
const static char * short_options = "hc:o:x:d:qv";
/// Command line long options.
const static struct option long_options[] =
{
    // Help
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 1 },

    // Logging and verbosity
    { "quiet", no_argument, NULL, 'q' },
    { "verbose", no_argument, NULL, 'v' },

    // Files and Directories
    { "core", required_argument , NULL, 'c' },
    { "outdir", required_argument , NULL, 'o' },
    { "xen-symtab", required_argument , NULL, 'x' },
    { "dom0-symtab", required_argument , NULL, 'd' },

    // EoL
    { NULL, 0, NULL, 0 }
};
/// Path to the xen symbol table.
static const char *xen_symtab_path;
/// Path to the dom0 symbol table.
static const char *dom0_symtab_path;
/// Default CORE crash file path.
static const char default_core_path[] = "/proc/vmcore";
/// Path to the CORE crash file.
static const char * core_path = default_core_path;
/// Log file path.
static const char * log_path = "xen-crashdump-analyser.log";
/// Path to the output directory
static const char * outdir_path = NULL;
/// Output directory descriptor
static int outdirfd = 0;
/// Working directory descriptor
static int workdirfd = 0;
/// xen.log descriptor
static FILE * xenfd = NULL;
/// Log file descriptor
static FILE * logfd = stderr;

/**
 * Convert a severity value to string
 * @param severity Severity value
 * @returns String representing the severity
 */
static const char * severity2str(int severity)
{
    switch ( severity )
    {
    case 0: return "ERROR";
    case 1: return "INFO ";
    default:
    case 2: return "DEBUG";
    case 3: return "DEBUG(refs)";
    }
}

/// Additional error file descriptor for logging.
static FILE * log_error = NULL;
void set_additional_log(FILE * fd) { log_error = fd; }

void __log(int severity, const char * ref, const char * fnc, const char * fmt, ...)
{
    static char buffer[256];
    va_list vargs;
    va_start(vargs, fmt);

    vsnprintf(buffer, sizeof buffer - 1, fmt, vargs);

    if ( severity <= verbosity )
    {
        if ( verbosity > 2 )
        {
            fprintf(logfd, "%s (%s:%s()) %s", severity2str(severity),
                    ref, fnc, buffer);
            if ( log_error && ! severity )
                fprintf(log_error, "%s (%s:%s()) %s", severity2str(severity),
                    ref, fnc, buffer);
        }
        else
        {
            fprintf(logfd, "%s %s", severity2str(severity), buffer);
            if ( log_error && ! severity )
                fprintf(log_error, "%s %s", severity2str(severity), buffer);
        }
    }

    if ( ! severity && (stderr != logfd))
        fprintf(stderr, "%s %s", severity2str(severity), buffer);

    va_end(vargs);
}

/// Atexit function to close the log file descriptor
void atexit_close_log( void )
{
    if ( logfd && ( logfd != stderr ) )
    {
        fflush ( logfd );
        fclose ( logfd );
        logfd = NULL;
    }
}

FILE * fopen_in_outdir(const char * path, const char * flags)
{
    FILE * fd = NULL;

    if ( -1 == fchdir( outdirfd ) )
    {
        LOG_ERROR("Failed to change to the output directory: %s\n",
                  strerror(errno));
        return NULL;
    }

    fd = fopen(path, flags);

    if ( -1 == fchdir( workdirfd ) )
    {
        LOG_ERROR("Failed to change to working directory: %s\n",
                  strerror(errno));
    }

    return fd;
}

/**
 * Print name and version.
 * @param stream Stream to write to.
 */
static void version(FILE * stream = stdout)
{
    fprintf(stream, "Xen Crashdump Analyser, version %s\n", version_str);
}

/**
 * Print usage information.
 * @param argv0 argv[0] from main() so usage can be printed with the.
 * correct context.
 * @param stream Stream to write the usage to.
 */
static void usage(char * argv0, FILE * stream = stdout)
{
    version(stream);
    fprintf(stream, "Usage: %s [options]\n\n", argv0);
    fprintf(stream, "Analyse a Xen crash in the kdump environment\n\n");
    fprintf(stream, "Options: (* indicates required)\n\n");

// @cond - Doxygen ought to ignore these macros.
//         They are for pretty-printing the command line parameters
#define WL 12
#define L_REQ(l,d)    fprintf(stream, "    --%-*s    * %s\n", WL, l, d);
#define LS_REQ(l,s,d) fprintf(stream, "    --%-*s -%c * %s\n", WL, l, s, d);
#define L_OPT(l,d)    fprintf(stream, "    --%-*s      %s\n", WL, l, d);
#define LS_OPT(l,s,d) fprintf(stream, "    --%-*s -%c   %s\n", WL, l, s, d);

    LS_OPT("core", 'c', "Core crash file.  Defaults to /proc/vmcore.");
    LS_REQ("outdir", 'o', "Directory for output files.");
    LS_REQ("xen-symtab", 'x', "Xen Symbol Table file.");
    LS_REQ("dom0-symtab", 'd', "Dom0 Symbol Table file.");
    putc('\n', stream);

    LS_OPT("help", 'h', "This description.");
    L_OPT("version", "Display version and exit.");
    putc('\n', stream);

    LS_OPT("quite", 'q', "Less logging.");
    LS_OPT("verbose", 'v', "More logging, accepted multiple times for extra debug logging.");
    putc('\n', stream);

#undef L_REQ
#undef LS_REQ
#undef L_OPT
#undef LS_OPT
#undef WL
// @endcond
}

/**
 * Parse the command line arguments.
 * @param argc Command line argument count
 * @param argv Command line arguments.
 * @returns boolean indicating whether the program should continue
 */
static bool parse_commandline(int argc, char ** argv)
{
    int opt_index = 0, current = 0;

    bool have_xen_symtab = false;
    bool have_dom0_symtab = false;
    bool have_outdir = false;

    /* Show help if no command line parameters presented, rather than failing
     * with an error about unspecified parameters. */
    if ( argc == 1 )
    {
        usage(argv[0]);
        return false;
    }

    while ( current != -1 )
    {
        current = getopt_long(argc, argv, short_options,
                              long_options, &opt_index);

        switch ( current )
        {
        case -1: // No more options
            break;

        case 1: // --version
            version();
            return false;
            break;

        case 'c': // CORE crash file
            core_path = optarg;
            break;

        case 'o': // output directory
            outdir_path = optarg;
            have_outdir = true;
            break;

        case 'x': // xen symtab
            xen_symtab_path = optarg;
            have_xen_symtab = true;
            break;

        case 'd': // dom0 symtab
            dom0_symtab_path = optarg;
            have_dom0_symtab = true;
            break;

        case 'q': // quiet
            verbosity = verbosity ? verbosity - 1 : verbosity;
            break;

        case 'v': // verbose
            verbosity = verbosity - 3 ? verbosity + 1 : verbosity;
            break;

        case 'h': // Help
        default: // Unrecognised
            usage(argv[0]);
            return false;
        }
    }

    if ( ! have_outdir )
    {
        printf("Required parameter {--outdir,-o} not found\n");
        return false;
    }

    if ( ! have_xen_symtab )
    {
        printf("Required parameter {--xen-symtab,-x} not found\n");
        return false;
    }

    if ( ! have_dom0_symtab )
    {
        printf("Required parameter {--dom0-symtab,-d} not found\n");
        return false;
    }

    return true;
}

/**
 * Main function.
 * @param argc Command line argument count
 * @param argv Command line arguments.
 */
int main(int argc, char ** argv)
{
    char * path_buff = NULL;
    Elf * elf = NULL;

    // Low memory environment - chances of getting std::bad_alloc are high
    try
    {
        // Log to stderr while we have no real file to log to
        logfd = stderr;

        // Parse the command line
        if ( ! parse_commandline(argc, argv) )
            return EX_USAGE;

        // Make the output dir if it doesn't exist
        if ( 0 > mkdir(outdir_path, 0700) )
        {
            if ( errno != EEXIST )
            {
                LOG_ERROR("Unable to create output directory \"%s\": %s\n",
                          outdir_path, strerror(errno));
                return EX_IOERR;
            }
        }

        // Get a handle to the current working directory
        if ( 0 > (workdirfd = open(".", O_RDONLY )))
        {
            LOG_ERROR("Unable to open working directory \"%s\": %s\n",
                      outdir_path, strerror(errno));
            return EX_IOERR;
        }

        // Get a handle to the output directory
        if ( 0 > (outdirfd = open( outdir_path, O_RDONLY )))
        {
            LOG_ERROR("Unable to open output directory \"%s\": %s\n",
                      outdir_path, strerror(errno));
            return EX_IOERR;
        }

        // Try and open the logging file
        if ( NULL == (logfd = fopen_in_outdir(log_path, "w")))
        {
            LOG_ERROR("Unable to open log file\n");
            return EX_IOERR;
        }

        // Ensure the log file gets closed if we return early
        if ( atexit(atexit_close_log) )
        {
            LOG_ERROR("call to atexit failed.  Something is very wrong\n");
            fclose(logfd);
            return EX_SOFTWARE;
        }

        // Apply line buffering to the log file
        if ( setvbuf(logfd, NULL, _IOLBF, 1024) )
        {
            LOG_ERROR("Unable to use line buffering mode for logging\n");
            return EX_IOERR;
        }

        LOG_INFO("Logging level is %s\n", severity2str(verbosity));

        // Log the command line to logfd
        if ( verbosity > 0 )
        {
            LOG_INFO("Command line:");
            for ( int x = 0; x < argc; ++x )
                fprintf(logfd, " %s", argv[x]);
            fputc('\n', logfd);
        }

        LOG_DEBUG("Opened log file '%s'\n", log_path);

        // Log the output directory
        if ( NULL == ( path_buff = realpath( outdir_path, NULL )))
        {
            LOG_ERROR("realpath failed for output directory '%s': %s\n",
                      outdir_path, strerror(errno));
            free(path_buff);
            return EX_SOFTWARE;
        }
        LOG_INFO("Output directory: %s/\n", path_buff);
        free(path_buff);

        // Log the xen symtab
        if ( NULL == ( path_buff = realpath( xen_symtab_path, NULL )))
        {
            LOG_ERROR("realpath failed for Xen symbol table path '%s': %s\n",
                      xen_symtab_path, strerror(errno));
            return EX_SOFTWARE;
        }
        LOG_INFO("Xen symbol table: %s\n", path_buff);
        free(path_buff);

        // Parse Xens symbol file
        if ( ! host.symtab.parse(xen_symtab_path, true) )
        {
            LOG_ERROR("  Failed to parse the Xen symbol table file\n");
            return EX_IOERR;
        }

        // Log the dom0 symtab
        if ( NULL == ( path_buff = realpath( dom0_symtab_path, NULL )))
        {
            LOG_ERROR("realpath failed for Dom0 symbol table path '%s': %s\n",
                      dom0_symtab_path, strerror(errno));
            return EX_SOFTWARE;
        }
        LOG_INFO("Dom0 symbol table: %s\n", path_buff);
        free(path_buff);

        // Parse dom0s symbol file
        if ( ! host.dom0_symtab.parse(dom0_symtab_path) )
        {
            LOG_ERROR("  Failed to parse the Xen symbol table file\n");
            return EX_IOERR;
        }

        // Log the crash file
        if ( NULL == ( path_buff = realpath( core_path, NULL )))
        {
            LOG_ERROR("realpath failed for Core crash file path '%s': %s\n",
                      core_path, strerror(errno));
            free(path_buff);
            return EX_SOFTWARE;
        }
        LOG_INFO("Elf CORE crash file: %s\n", path_buff);
        free(path_buff);

        // Evaluate what kind of elf file we have
        if ( NULL == (elf = Elf::create(core_path)) )
        {
            LOG_ERROR("  Failed to parse the crash file\n");
            return EX_IOERR;
        }

        // Parse the program headers and notes
        if ( ! elf->parse() )
        {
            LOG_ERROR("  Failed to parse the crash file\n");
            SAFE_DELETE(elf);
            return EX_IOERR;
        }

        // Populate the memory regions
        if ( ! memory.setup(core_path, elf) )
        {
            LOG_ERROR("  Failed to set up memory regions from crash file\n");
            SAFE_DELETE(elf);
            return EX_SOFTWARE;
        }

        // Set up the host structures
        if ( ! host.setup(elf) )
        {
            LOG_ERROR("  Failed to set up host structures\n");
            SAFE_DELETE(elf);
            return EX_SOFTWARE;
        }

        SAFE_DELETE(elf);

        // Try to open the xen-console-ring.log file
        if ( NULL == (xenfd = fopen_in_outdir("xen.log", "w")))
        {
            LOG_ERROR("Unable to open xen.log in output directory: %s\n", strerror(errno));
            return EX_IOERR;
        }
        LOG_INFO("Opened xen.log for host information\n");

        /* This ordering looks a little suspect, but it allows processing of the
         * subsequent work iff the previous work succeeds, along with fallthrough
         * error logic without gotos or returns. */
        if ( ! host.decode_xen() )
            LOG_ERROR("  Failed to decode xen structures\n");
        else if ( ! host.print_xen(xenfd) )
            LOG_ERROR("  Failed to print xen information\n");
        else
        {
            int s = host.print_domains();
            LOG_DEBUG("Successfully printed %d domains\n", s);
        }
    }
    catch ( const std::bad_alloc & )
    {
        LOG_ERROR("Caught bad_alloc.  Not enough memory\n");
        return EX_SOFTWARE;
    }
    catch ( ... )
    {
        // This should never be caught, but just to be on the safe side
        LOG_ERROR("Catch wildcard triggered in %s:%d\n", __func__, __LINE__);
        abort();
    }

    fclose(xenfd);
    LOG_INFO("COMPLETE\n");
    return EX_OK;
}

/*
 * Local variables:
 * mode: C++
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
