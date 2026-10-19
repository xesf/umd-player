#ifndef INFOSHEET_H
#define INFOSHEET_H

/* UMD Archive Collective info sheets: the submission template filled with what a disc image
   tells about itself (title, type, serial, languages, subtitles, aspect ratio, size and
   checksums), the rest left for the dumper to fill in. One plain-text file per image, in a
   "metadata" folder beside it. */

/* "<folder>/metadata/<image name without extension>.txt" */
void infosheet_path(const char *image, char *out, int size);

/* Reads the image and writes its sheet. While the checksums run, *permille goes from 0 to 1000,
   and a nonzero *stop gives up (no file written). Returns 0, or -1 on a read or write error or
   when stopped. */
int infosheet_write(const char *image, volatile int *stop, volatile int *permille);

#endif
