#ifndef ELFEED_APP_HPP
#define ELFEED_APP_HPP

#include <wx/app.h>
#include <wx/snglinst.h>

#include <memory>

#include "elfeed.hpp"

class MainFrame;

class ElfeedApp : public wxApp {
public:
    bool OnInit() override;
    int OnExit() override;

    Elfeed &state() { return state_; }
    MainFrame *main_frame() { return main_frame_; }

private:
    Elfeed state_;
    MainFrame *main_frame_ = nullptr;
    std::unique_ptr<wxSingleInstanceChecker> instance_checker_;
};

wxDECLARE_APP(ElfeedApp);

#endif
