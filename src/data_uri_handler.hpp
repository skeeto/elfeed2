#ifndef ELFEED_DATA_URI_HANDLER_HPP
#define ELFEED_DATA_URI_HANDLER_HPP

#include <wx/filesys.h>

// wxHtmlWindow resolves <img src> through wxFileSystem. wxWidgets 3.2
// ships no built-in handler for the "data:" scheme, so inlined images
// would silently drop. Register this handler once at app startup
// (wxFileSystem::AddHandler) to make data-URIs work.
class DataURIHandler : public wxFileSystemHandler {
public:
    bool CanOpen(const wxString &location) override;
    wxFSFile *OpenFile(wxFileSystem &fs, const wxString &location) override;
};

#endif
