/*
 * 	    pstoqpdl.cpp              (C) 2007-2008, Aurélien Croc (AP²C)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 *  $Id$
 * 
 */
#include "ppdfile.h"
#include "errlog.h"
#include "version.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>


/*
 * Appel des filtres
 * Filter call
 */
static char *_toLower(const char *data)
{
    char *tmp = new char[strlen(data) + 1];
    unsigned int i;

    for (i=0; data[i]; i++)
        tmp[i] = (char) tolower(data[i]);
    tmp[i] = 0;
    return tmp;
}

static int _linkFilters(const char *arg1, const char *arg2, const char *arg3,
    const char *arg4, const char *arg5) 
{
    int rasterInput[2], rasterOutput[2];
    int raster, splix;

    // Call pstoraster
    if (pipe(rasterInput) || pipe(rasterOutput)) {
        ERRORMSG(_("Cannot create pipe (%i)"), errno);
        return 0;
    }

    // Launch SpliX
    if (!(splix = fork())) {
        // SpliX code
        close(rasterInput[1]);
        close(rasterInput[0]);
        close(rasterOutput[1]);
        dup2(rasterOutput[0], STDIN_FILENO);
        close(rasterOutput[0]);
        execl(RASTERDIR "/" RASTERTOQPDL, RASTERDIR "/" RASTERTOQPDL, arg1, 
            arg2, arg3, arg4, arg5, (char *)NULL);
        ERRORMSG(_("Cannot execute rastertoqpdl (%i)"), errno);
        exit(0);
    }
    DEBUGMSG(_("SpliX launched with PID=%u"), splix);
    
    // Launch the raster
    dup2(rasterInput[1], STDOUT_FILENO);
    close(rasterOutput[0]);
    close(rasterInput[1]);
    if (!(raster = fork())) {
        // Raster code
        dup2(rasterInput[0], STDIN_FILENO);
        dup2(rasterOutput[1], STDOUT_FILENO);
        close(rasterInput[0]);
        close(rasterOutput[1]);
        if (access(RASTERDIR "/" GSTORASTER, F_OK) != -1) {
            // gstoraster filter exists
            execl(RASTERDIR "/" GSTORASTER, RASTERDIR "/" GSTORASTER, arg1, arg2, 
                arg3, arg4, arg5,(char *)NULL);
            ERRORMSG(_("Cannot execute gstoraster (%i)"), errno);
        } else {
            // use pstoraster if gstoraster doesn't exist
            execl(RASTERDIR "/" PSTORASTER, RASTERDIR "/" PSTORASTER, arg1, arg2, 
                arg3, arg4, arg5,(char *)NULL);
            ERRORMSG(_("Cannot execute %s (%i)"), PSTORASTER, errno);
        }
        exit(0);
    }
    DEBUGMSG(_("raster launched with PID=%u"), raster);
    close(rasterInput[0]);
    close(rasterOutput[1]);

    return splix;
}



/*
 * Lecture des fichiers CRD / CSA
 * CSA / CRD read
 */
static char *_readCMSFile(PPDFile& ppd, const char *manufacturer, bool csa)
{
    unsigned long xResolution=0, yResolution=0, size;
    PPDValue resolution;
    const char *file;
    char *tmp, *res;
    struct stat fi;
    FILE *handle;

    // Get the base filename
    file = ppd.get("CMSFile", "General");
    if (!file || !(*file))
        return NULL;

    // Get the resolution
    resolution = ppd.get("Resolution");
    if (resolution == "1200dpi")
        xResolution = yResolution = 1200;
    else if (resolution == "600dpi")
        xResolution = yResolution = 600;
    else if (resolution == "1200x600dpi") {
        xResolution = 1200;
        yResolution = 600;
    } else if (resolution == "300dpi") 
        xResolution = yResolution = 300;

    // Get the real filename
    size = strlen(CUPSPROFILE) + strlen(manufacturer) + strlen(file) + 64;
    tmp = new char[size];
    if (xResolution)
        snprintf(tmp, size, CUPSPROFILE "/%s/%s-%lux%lucms%s", manufacturer,
            file, xResolution, yResolution, csa ? "2" : "");
    if (!xResolution || access(tmp, R_OK))
        snprintf(tmp, size, CUPSPROFILE "/%s/%scms%s", manufacturer,
            file, csa ? "2" : "");

    // Check if it exists, open it and read it
    if (stat(tmp, &fi) || !(handle = fopen(tmp, "r"))) {
        ERRORMSG(_("Cannot open CMS file %s (%i)"), tmp, errno);
        delete[] tmp;
        return NULL;
    }
    if (!fi.st_size) {
        ERRORMSG(_("CMS file %s is empty"), tmp);
        delete[] tmp;
        fclose(handle);
        return NULL;
    }
    res = new char[fi.st_size + 1];
    if (!fread(res, 1, fi.st_size, handle)) {
        ERRORMSG(_("Cannot read CMS file %s (%i)"), tmp, errno);
        delete[] tmp;
        delete[] res;
        fclose(handle);
        return NULL;
    }
    res[fi.st_size] = 0;
    fclose(handle);
    delete[] tmp;

    return res;
}



/*
 * PROGRAMME PRINCIPAL
 * MAIN ROUTINE
 */
int main(int argc, char **argv)
{
    /* const char *jobid, *user, *title; Not Used */
    const char *options, *ppdFile, *file;
    const char *paperType, *manufacturer;
    /* unsigned long copies; Not Used */
    bool pageSetup=false;
    char buffer[1024];
    char *crd, *csa;
    int pid, err;
    PPDFile ppd;

    // Check the given arguments
    if (argc != 6 && argc != 7) {
        fprintf(stderr, _("Usage: %s job-id user title copies options "
            "[file]\n"), argv[0]);
        return 1;
    }
    /* jobid = argv[1]; Not used */
    /* user = argv[2]; Not used */
    /* title = argv[3]; Not used */
    options = argv[5];
    file = argc == 7 ? argv[6] : NULL;
    /* copies = strtol(argv[4], (char **)NULL, 10); Not used */
    ppdFile = getenv("PPD");

    // Get more information on the SpliX environment (for debugging)
    DEBUGMSG(_("PS => SpliX filter V. %s by Aurélien Croc (AP²C)"), VERSION);
    DEBUGMSG(_("More information at: http://splix.ap2c.org"));

    // Open the given file
    if (file && !freopen(file, "r", stdin)) {
        ERRORMSG(_("Cannot open file %s"), file);
        return errno;
    }

    // Open the PPD file and get paper information
    if (!ppd.open(ppdFile, PPDVERSION, options))
        return 1;
    manufacturer = _toLower(ppd.get("Manufacturer"));
    paperType = ppd.get("MediaType");
    if (!(strcasecmp(paperType, "OFF")))
        paperType = "NORMAL";

    // Call the other filters
    if (!(pid = _linkFilters(argv[1], argv[2], argv[3], argv[4], argv[5]))) {
        ERRORMSG(_("Filter error.. Cannot continue"));
        delete[] manufacturer;
        return 1;
    }

    // Get the CRD and CSA information and send the PostScript data
    crd = _readCMSFile(ppd, manufacturer, false);
    csa = _readCMSFile(ppd, manufacturer, true);
    if (!crd || !csa) {
        WARNMSG(_("CMS data are missing. Color correction aborted"));
        if (crd) {
            delete[] crd;
            crd = NULL;
        }
        if (csa) {
            delete[] csa;
            csa = NULL;
        }
        while (!(feof(stdin))) {
            fgets((char *)&buffer, sizeof(buffer), stdin);
            fprintf(stdout, "%s", (char *)&buffer); 
        }
    } else {

        // Insert the MediaChoice and colour correction information into
        // the postscript header.
        //
        // Look for a "%%Creator" line in the postscript header, and
        // insert the information before it.
        //
        // Postscript that is created by pdftops from content from Apple 
        // iOS devices (iPad etc) seems not to have a "%%Creator" line in
        // the header, so we insert a dummy one. Without this, pstoqpdl
        // seems to crash ghostscript.
        //
        // NB: according to the PostScript Document Structuring Conventions
        // (DSC) specification the end of the postscript header should be
        // the "%%EndComments" line - see:
        // http://partners.adobe.com/public/developer/en/ps/5001.DSC_Spec.pdf


        // search each line in the postscript header
        while (!(feof(stdin))) {

            // read a line of the input file
            if (!fgets((char *)&buffer, sizeof(buffer), stdin))
                break;

            if (!(memcmp("%%Creator", (char *)&buffer, 9)) ||
                !(memcmp("%%LanguageLevel:", (char *)&buffer, 16))) {
                // found a "%%Creator" line

                // emit the MediaChoice and colour correction information
                if (paperType)
                    fprintf(stdout, "/MediaChoice (%s) def\n", paperType);
                fprintf(stdout, "%s", crd);
                fprintf(stdout, "%s", csa);

                // emit the original "%%Creator" line
                fprintf(stdout, "%s", (char *)&buffer); 

                // stop scanning the header
                break;
            }


            if (!(memcmp("%%EndComments", (char *)&buffer, 13))) {
                // reached end of header without finding a "%%Creator" line

                // emit the MediaChoice and colour correction information
                if (paperType)
                    fprintf(stdout, "/MediaChoice (%s) def\n", paperType);
                fprintf(stdout, "%s", crd);
                fprintf(stdout, "%s", csa);

                // emit a dummy "%%Creator" line
                DEBUGMSG(_("inserting dummy \"Creator\" entry in postscript header"));
                fprintf(stdout, "%s", "%%Creator: SpliX pstoqpdl filter");

                // emit the original "%%EndComments" line
                fprintf(stdout, "%s", (char *)&buffer);

                // stop scanning the header
                break;
            }


            if (!(memcmp("%%BeginPro", (char *)&buffer, 10)) ||
                !(memcmp("%%BeginRes", (char *)&buffer, 10))) {
                // we shouldn't find either of these lines in the header

                ERRORMSG(_("End of PostScript header not found"));

                // emit the line that was found
                fprintf(stdout, "%s", (char *)&buffer); 

                // stop scanning the header
                break;
            }

            // encountered some other kind of header line - just emit it
            fprintf(stdout, "%s", (char *)&buffer); 
        }


        // Check for each page
        while (!(feof(stdin))) {
            if (!fgets((char *)&buffer, sizeof(buffer), stdin))
                break;
            if (!(memcmp("%%Page:", (char *)&buffer, 7))) {
                char tmp[sizeof(buffer)];

                if (!fgets((char *)&tmp, sizeof(tmp), stdin)) {
                    fprintf(stdout, "%s", (char *)&buffer);
                    break;
                }
                if (!(memcmp("%%BeginPageSetup", (char *)&tmp, 16)))
                    pageSetup = true;
                else
                    fprintf(stdout, "%s", csa);
                fprintf(stdout, "%s", (char *)&buffer);
                fprintf(stdout, "%s", (char *)&tmp);
            } else if (pageSetup && !(memcmp("%%EndPageSetup", 
                (char *)&buffer, 14))) {
                fprintf(stdout, "%s", (char *)&buffer);
                fprintf(stdout, "%s", csa);
                pageSetup = false;
            } else 
                fprintf(stdout, "%s", (char *)&buffer);
        }
    }


    // Close the output and wait for Splix to be finished
    fclose(stdout);
    waitpid(pid, &err, 0);

    if (crd)
        delete[] crd;
    if (csa)
        delete[] csa;
    if (manufacturer)
        delete[] manufacturer;
    return WEXITSTATUS(err);
}

/* vim: set expandtab tabstop=4 shiftwidth=4 smarttab tw=80 cin enc=utf8: */

