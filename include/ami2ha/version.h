/*
 * ami2ha -- one place for the version
 *
 * Everything that names a version reads it from here: the window title,
 * the About box, the ARexx VERSION command, the $VER string the AmigaDOS
 * Version command looks for, and the name of the release archive.
 */
#ifndef AMI2HA_VERSION_H
#define AMI2HA_VERSION_H

#define A2H_NAME         "ami2ha"
#define A2H_VERSION      "0.2"
#define A2H_VERSION_DATE "16.8.2026"

#define A2H_TITLE        A2H_NAME " " A2H_VERSION

/*
 * The AmigaDOS Version command searches a binary for this exact shape.
 * Keep the "$VER: " prefix and the spacing.
 */
#define A2H_VERSTAG \
    "$VER: " A2H_NAME " " A2H_VERSION " (" A2H_VERSION_DATE ")"

#endif /* AMI2HA_VERSION_H */
