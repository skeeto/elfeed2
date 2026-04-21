#include "data_uri_handler.hpp"

#include <wx/base64.h>
#include <wx/datetime.h>
#include <wx/mstream.h>
#include <wx/uri.h>

// RFC 2397 data URI:
//
//     data:[<mime>][;base64],<payload>
//
// We only handle the base64 form (that's what image_cache produces)
// but also accept percent-encoded payloads in case someone hand-writes
// a small svg/xml data URI into feed content. Non-base64 + non-
// percent-encoded text is treated as a literal byte stream — same as
// browsers do.

bool DataURIHandler::CanOpen(const wxString &location)
{
    return GetProtocol(location) == "data";
}

wxFSFile *DataURIHandler::OpenFile(wxFileSystem & /*fs*/,
                                   const wxString &location)
{
    // Strip the "data:" scheme. location may or may not be URL-quoted
    // depending on how wxFileSystem got here; we do our own parsing
    // rather than trusting wxURI which mis-decodes base64 payloads.
    if (!location.StartsWith("data:")) return nullptr;
    wxString rest = location.AfterFirst(':');

    int comma = rest.Find(',');
    if (comma == wxNOT_FOUND) return nullptr;
    wxString prefix = rest.Left(comma);          // e.g. "image/png;base64"
    wxString payload = rest.Mid(comma + 1);

    wxString mime = prefix.BeforeFirst(';');
    if (mime.empty()) mime = "text/plain";       // RFC 2397 default

    // Decode the payload. wxMemoryBuffer owns the bytes; wxFSFile
    // wraps a wxMemoryInputStream over a copy. The stream takes
    // ownership of its backing buffer, so we pass a fresh allocation.
    void *buf = nullptr;
    size_t len = 0;
    bool ok = false;

    if (prefix.Lower().Contains(";base64")) {
        wxMemoryBuffer decoded = wxBase64Decode(payload);
        len = decoded.GetDataLen();
        if (len > 0) {
            buf = malloc(len);
            if (buf) {
                memcpy(buf, decoded.GetData(), len);
                ok = true;
            }
        }
    } else {
        // Percent-decode into a byte buffer.
        wxURI u("data:," + payload);
        wxString decoded = u.GetPath();
        wxScopedCharBuffer utf8 = decoded.utf8_str();
        len = utf8.length();
        if (len > 0) {
            buf = malloc(len);
            if (buf) {
                memcpy(buf, utf8.data(), len);
                ok = true;
            }
        }
    }

    if (!ok) {
        free(buf);
        return nullptr;
    }

    // wxMemoryInputStream in "read from existing buffer" mode doesn't
    // own the buffer, so we need a wrapper that frees it on destroy.
    // Easiest: copy into a member wxMemoryInputStream constructed from
    // a wxMemoryOutputStream. But a simpler path: use wxMemoryFSFile
    // semantics — construct the stream over our malloc'd buffer and
    // let it leak until wxFSFile is destroyed. That's still a leak;
    // instead, use wxMemoryInputStream's ctor that takes ownership.
    //
    // Actually wxMemoryInputStream has no take-ownership ctor. Work
    // around by subclassing to free the buffer in the destructor.
    class OwningMemoryStream : public wxMemoryInputStream {
    public:
        OwningMemoryStream(void *data, size_t len)
            : wxMemoryInputStream(data, len), data_(data) {}
        ~OwningMemoryStream() override { free(data_); }
    private:
        void *data_;
    };

    auto *stream = new OwningMemoryStream(buf, len);
    return new wxFSFile(stream, location, mime, wxEmptyString,
                        wxDateTime::Now());
}
