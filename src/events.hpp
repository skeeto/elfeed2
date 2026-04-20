#ifndef ELFEED_EVENTS_HPP
#define ELFEED_EVENTS_HPP

#include <wx/event.h>

// Worker threads post this to MainFrame (via wxQueueEvent) when state
// changes — new fetch results in the inbox, download progress, log lines
// appended, etc. The handler re-reads the relevant fields and refreshes
// the UI. One event type for all wakes; cheap to post.
wxDECLARE_EVENT(wxEVT_ELFEED_WAKE, wxThreadEvent);

#endif
