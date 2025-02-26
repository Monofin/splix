/*
 * 	    compress.cpp              (C) 2006-2008, Aurélien Croc (AP²C)
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
#include "compress.h"
#include <math.h>
#include <string.h>
#include "page.h"
#include "band.h"
#include "errlog.h"
#include "request.h"
#include "bandplane.h"

#include "algo0x0d.h"
#include "algo0x0e.h"
#include "algo0x11.h"
#include "algo0x13.h"
#include "algo0x15.h"

static bool _isEmptyBand(unsigned char* band, unsigned long size)
{
    unsigned long max, mod;

    max = size / sizeof(unsigned long);
    mod = size % sizeof(unsigned long);

    for (unsigned long i=0; i < max; i++) {
        if (((unsigned long*)band)[i])
            return false;
    }
    for (unsigned long i=0; i < mod; i++)
        if (band[size-i-1])
            return false;
    return true;
}

static bool _compressBandedPage(const Request& request, Page* page)
{
    unsigned long index=0, pageHeight, pageWidth, lineWidthInB, bandHeight;
    unsigned long bandSize, hardMarginX, hardMarginXInB, hardMarginY;
    unsigned char *planes[4], *band;
    unsigned long bandNumber=0;
    unsigned char colors;

    colors = page->colorsNr();
    hardMarginX = ((unsigned long)ceil(page->convertToXResolution(request.
        printer()->hardMarginX())) + 7) & ~7;
    hardMarginY = ceil(page->convertToYResolution(request.printer()->
        hardMarginY()));
    hardMarginXInB = hardMarginX / 8;
    pageWidth = page->width();
    pageHeight = page->height() - hardMarginY;
    page->setWidth(pageWidth);
    page->setHeight(pageHeight);
    lineWidthInB = (pageWidth + 7) / 8;
    bandHeight = request.printer()->bandHeight();
    if (page->xResolution() == 300 && page->yResolution() == 300)
        bandHeight /= 2;
    bandSize = lineWidthInB * bandHeight;
    index = hardMarginY * lineWidthInB;
    band = new unsigned char[bandSize];
    for (unsigned int i=0; i < colors; i++)
        planes[i] = page->planeBuffer(i);

    /*
     * 1. On vérifier si la bande n'est pas blanche
     *      |-> Si bande blanche, on passe
     *      '-> Sinon, on compresse
     * 2. On rajoute les informations de bande (numéro de bande et de
     *    couleur).
     * 3. On enregistre la bande dans la page.
     * 4. On détruit les buffers de plans dans la page.
     */
    while (pageHeight) {
        unsigned long localHeight = bandHeight;
        Band *current = NULL;
        bool theEnd = false;

        // Special things to do for the last band
        if (pageHeight < bandHeight) {
            theEnd = true;
            localHeight = pageHeight;
            memset(band, 0, bandSize);
        }

        for (unsigned int i=0; i < colors; i++) {
            Algorithm *algo = NULL;
            BandPlane *plane;

            switch (page->compression()) {
                case 0x0D:
                    algo = new Algo0x0D;
                    break;
                case 0x0E:
                    algo = new Algo0x0E;
                    break;
                case 0x11:
                    algo = new Algo0x11;
                    break;
                default:
                    ERRORMSG(_("Unknown compression algorithm. Aborted"));
                    return false;
            }

            // Copy the data into the band depending on the algorithm options
            if (algo->reverseLineColumn()) {
                for (unsigned int y=0; y < localHeight; y++) {
                    for (unsigned int x=0; x < lineWidthInB - hardMarginXInB; x++) {
                        band[x * bandHeight + y] = planes[i][index + x +
                            hardMarginXInB + y * lineWidthInB];
                    }
                    for (unsigned int x=lineWidthInB - hardMarginXInB; x < lineWidthInB; x++)
                        band[x * bandHeight + y] = 0;
                }
            } else {
                for (unsigned int y=0; y < localHeight; y++) {
                    for (unsigned int x=0; x < lineWidthInB - hardMarginXInB; x++) {
                        band[x + y * lineWidthInB] = planes[i][index + x +
                            hardMarginXInB + y * lineWidthInB];
                    }
                    for (unsigned int x=lineWidthInB - hardMarginXInB; x < lineWidthInB; x++)
                        band[x + y * lineWidthInB]  = 0;
                }
            }

            // Does the band is empty?
            if (_isEmptyBand(band, bandSize)) {
                delete algo;
                continue;
            }

            // Check if bytes have to be reversed
            if (algo->inverseByte())
                for (unsigned int j=0; j < bandSize; j++)
                    band[j] = ~band[j];

            // Call the compression method
            plane = algo->compress(request, band, pageWidth, bandHeight);
            /*
             * If algorithm 0xd did not create a plane, it means that the 
             * complementary algorithm 0xE need to be used
             */
            if (!plane && page->compression() == 0x0D) {
                delete algo;
                algo = new Algo0x0E;
                /* Bytes has to be reversed first, as algo0xd didn't do that. */
                for (unsigned int j = 0; j < bandSize; j++)
                    band[j] = ~band[j];
                /* Do the encoding with algo0xe. */
                plane = algo->compress(request, band, pageWidth, bandHeight);
            }

            if (plane) {
                plane->setColorNr(i + 1);
                if (!current)
                    current = new Band(bandNumber, pageWidth, bandHeight);
                current->registerPlane(plane);
            }

            delete algo;
        }
        if (current)
            page->registerBand(current);
        bandNumber++;
        index += bandSize;
        pageHeight = theEnd ? 0 : pageHeight - bandHeight;
    }
    page->flushPlanes();
    delete[] band;

    return true;
}

#ifndef DISABLE_JBIG
static bool _compressBandedJBIGPage(const Request& request, Page* page)
{
    unsigned long index=0, pageHeight, lineWidthInB, bandHeight = 128;
    unsigned long bufferWidth, bandSize, hardMarginXInB=13, hardMarginY=100;
    unsigned char *planes[4], *band[4];
    unsigned long bandNumber=0, xLimitInB, bufferWidthInB;
    Algo0x15 *algo = new Algo0x15;
    /* Image trimming are done from hardware margins defined in the ppd. */
    hardMarginXInB = ((unsigned long)ceil(page->convertToXResolution(request.
        printer()->hardMarginX())) + 7) / 8;
    hardMarginY = ceil(page->convertToYResolution(request.printer()->
        hardMarginY()));
    bandHeight = request.printer()->bandHeight();
    lineWidthInB = ((page->width()) + 7) / 8;
    // Compute an updated page height, clipped on top.
    pageHeight = page->height() - hardMarginY; 
    // Compute the buffer width that is nearest multiple of 256 to the original. 
    bufferWidth = page->width() & ~255;
    if ((bufferWidth + 128) < page->width())
      bufferWidth += 256;
    // Update the page heigth.
    page->setHeight(pageHeight);
    // Update the page width.
    page->setWidth(bufferWidth);
    bufferWidthInB = (bufferWidth + 7) / 8;
    bandSize = bufferWidthInB * bandHeight;
    index = hardMarginY * lineWidthInB;
    for (unsigned int i=0; i < page->colorsNr(); i++) {
        band[i] = new unsigned char[bandSize];
        planes[i] = page->planeBuffer(i);
    }
    /*
       Here, limit the width of the copied image to the buffer, as the buffer
       width varies and can lead to 6 practical cases, the following is a
       condensed code.
    */
    if (hardMarginXInB + bufferWidthInB < lineWidthInB - hardMarginXInB)
        xLimitInB = hardMarginXInB + bufferWidthInB;
    else
        xLimitInB = lineWidthInB - hardMarginXInB;
    bool theEnd = false;
    unsigned long indexSizeIncrement = bandHeight * lineWidthInB;
    unsigned long localHeight = bandHeight;
    while (pageHeight) {
        Band *current = NULL;
        bool cmyPlanesHasData = false;
        // Special things to do for the last band
        if (pageHeight < bandHeight) {
            theEnd = true;
            localHeight = pageHeight;
            for (unsigned int i=0; i < page->colorsNr(); i++)
                memset(band[i], 0, bandSize);
        }
        for (unsigned int i=0; i < page->colorsNr(); i++) {
            for (unsigned int y=0; y < localHeight; y++) {
                for (unsigned int x=hardMarginXInB; x < xLimitInB; x++)
                    band[i][x - hardMarginXInB + y * bufferWidthInB] =
                             planes[i][index + x + y * lineWidthInB];
                for (unsigned int x=lineWidthInB - 2 * hardMarginXInB;
                                  x < bufferWidthInB; x++)
                    band[i][x + y * bufferWidthInB] = 0;
            }
        }
        // Are the CMY planes completely empty in the band?

        for (int i=0; i < page->colorsNr() - 1; i++)
            if (!_isEmptyBand(band[i], bandSize)) { 
                cmyPlanesHasData = true;
                break;
            };
        // Compress the entire band.
        if (cmyPlanesHasData) {
            for (unsigned int i=0; i < page->colorsNr(); i++) {
                BandPlane *plane = algo->compress(request, band[i],
                                                  bufferWidth, bandHeight);
                if (plane) {
                    plane->setColorNr((1 == page->colorsNr()) ? 4:i + 1);
                    if (!current)
                        current = new Band(bandNumber, bufferWidth, bandHeight);
                    current->registerPlane(plane);
                }
            }
        } else if (!_isEmptyBand(band[page->colorsNr() - 1], bandSize)) { 
            // Compress only the K band.
            BandPlane *plane = algo->compress(request,
                                              band[page->colorsNr() - 1],
                                              bufferWidth, bandHeight);
            if (plane) {
                plane->setColorNr(4);
                if (!current)
                    current = new Band(bandNumber, bufferWidth, bandHeight);
                current->registerPlane(plane);
            }
        }
        if (current)
            page->registerBand(current);
        bandNumber++;
        index += indexSizeIncrement;
        pageHeight = theEnd ? 0 : pageHeight - bandHeight;
    }
    if (page->bandsNr() > 0)
        page->setBIH(algo->getBIHdata());
    page->flushPlanes();
    for (unsigned int i=0; i < page->colorsNr(); i++)
        delete[] band[i];
    delete algo;
    return true;
}

static bool _compressWholePage(const Request& request, Page* page)
{
    unsigned long hardMarginX, hardMarginXInB, hardMarginY, lineWidthInB;
    unsigned long pageWidth, bandHeight, planeHeight, pageHeight, index;
    unsigned long bandNumber=0;
    unsigned char *buffer;
    Band *current = NULL;
    Algo0x13 algo[4];

    hardMarginX = ((unsigned long)ceil(page->convertToXResolution(request.
        printer()->hardMarginX())) + 7) & ~7;
    hardMarginY = ceil(page->convertToYResolution(request.printer()->
        hardMarginY()));
    hardMarginXInB = hardMarginX / 8;
    pageWidth = page->width() - hardMarginX * 2;
    pageHeight = page->height() - hardMarginY * 2;
    page->setWidth(pageWidth);
    page->setHeight(pageHeight);
    lineWidthInB = (pageWidth + 7) / 8;
    bandHeight = request.printer()->bandHeight();
    // Alignment of the page height on band height
    planeHeight = ((pageHeight + bandHeight - 1) / bandHeight) * bandHeight;
    buffer = new unsigned char[lineWidthInB * planeHeight];

    do {
        current = NULL;
        for (unsigned int i=0; i < page->colorsNr(); i++) {
            unsigned char *curPlane = page->planeBuffer(i);
            BandPlane *plane;

            index = hardMarginY * (lineWidthInB + 2 * hardMarginXInB) + 
                hardMarginXInB;
            for (unsigned int y=0; y < pageHeight; y++, 
                    index += 2 * hardMarginXInB) {
                for (unsigned int x=0; x < lineWidthInB; x++, index++) {
                    buffer[x + y * lineWidthInB] = curPlane[index];
                }
            }
            memset(buffer + pageHeight * lineWidthInB, 0,
                   (planeHeight - pageHeight) * lineWidthInB);

            // Call the compression method
            plane = algo[i].compress(request, buffer, page->width(), 
                planeHeight);
            if (plane) {
                plane->setColorNr(i + 1);
                if (!current)
                    current = new Band(bandNumber, page->width(), 
                        request.printer()->bandHeight());
                current->registerPlane(plane);
            }
        }
        if (current)
            page->registerBand(current);
        bandNumber++;
    } while (current);

    page->flushPlanes();
    delete[] buffer;

    return true;
}
#endif /* DISABLE_JBIG */

bool compressPage(const Request& request, Page* page)
{
    switch(page->compression()) {
        case 0x0D:
        case 0x0E:
        case 0x11:
            return _compressBandedPage(request, page);
        case 0x13:
#ifndef DISABLE_JBIG
            return _compressWholePage(request, page);
#else
            ERRORMSG(_("J-BIG compression algorithm has been disabled during "
                "the compilation. Please recompile SpliX and enable the "
                "J-BIG compression algorithm."));
            break;
#endif /* DISABLE_JBIG */
        case 0x15:
#ifndef DISABLE_JBIG
            return _compressBandedJBIGPage(request, page);
#else
            ERRORMSG(_("J-BIG compression algorithm has been disabled during "
                "the compilation. Please recompile SpliX and enable the "
                "J-BIG compression algorithm."));
            break;
#endif /* DISABLE_JBIG */
        default:
            ERRORMSG(_("Compression algorithm 0x%lX does not exist"), 
                page->compression());
    }
    return false;
}

/* vim: set expandtab tabstop=4 shiftwidth=4 smarttab tw=80 cin enc=utf8: */

