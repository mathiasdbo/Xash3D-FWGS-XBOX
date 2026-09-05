#ifndef XISOBUILDER_H
#define XISOBUILDER_H

#include <QString>

/**
 * Handles packaging optimized asset files alongside default.xbe into an XISO disc image.
 */
class XisoBuilder {
public:
    XisoBuilder() = default;

    bool buildXiso(const QString& xbePath, const QString& mpakPath, const QString& outputPath) {
        // Stub: Combine default.xbe and MPAK into bootable XISO image
        return true;
    }
};

#endif // XISOBUILDER_H
