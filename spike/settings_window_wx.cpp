// Standalone wxWidgets look-check spike for the SideScopes settings window.
// Nothing here is wired to the app; the point is to compare native wxWidgets
// controls against the parallel Dear ImGui spike on macOS and Windows.

#include <wx/listctrl.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/wx.h>

#include <utility>
#include <vector>

class SettingsFrame : public wxFrame
{
public:
    SettingsFrame();

private:
    wxStaticText* makeHeader(wxWindow* parent, const wxString& text);
    wxPanel* buildGeneralPage(wxWindow* parent);
    wxWindow* buildScopesPage(wxWindow* parent);
    wxPanel* buildShortcutsPage(wxWindow* parent);
    wxPanel* buildAboutPage(wxWindow* parent);
    void addScopeSection(wxWindow* page, wxSizer* parentSizer, const wxString& title,
                         const std::vector<std::pair<wxString, std::vector<wxString>>>& choices);

    void onSidebar(wxCommandEvent& event);
    void onAppearance(wxCommandEvent& event);
    void onRowActivated(wxListEvent& event);
    void onSwap(wxCommandEvent& event);
    void onCharHook(wxKeyEvent& event);

    int findKeyOwner(const wxString& key, long exceptRow) const;
    void startCapture(long row);
    void applyCaptureKey(const wxString& newKey);
    void setRowKey(long row, const wxString& key);
    void showConflict(const wxString& key, const wxString& otherAction);
    void hideConflict();
    void finishCapture();
    void cancelCapture();

    wxListBox* m_sidebar = nullptr;
    wxSimplebook* m_book = nullptr;
    wxChoice* m_appearance = nullptr;
    wxStaticText* m_appearanceHint = nullptr;
    wxPanel* m_shortcutsPage = nullptr;
    wxListCtrl* m_list = nullptr;
    wxStaticText* m_conflictText = nullptr;
    wxButton* m_swapButton = nullptr;

    std::vector<wxString> m_actions;
    std::vector<wxString> m_keys;
    long m_captureRow = -1;
    int m_conflictOther = -1;
    wxString m_savedKey;
    wxString m_pendingKey;
};

class SpikeApp : public wxApp
{
public:
    bool OnInit() override;
};

wxDECLARE_APP(SpikeApp);

SettingsFrame::SettingsFrame()
    : wxFrame(nullptr, wxID_ANY, "Settings", wxDefaultPosition, wxSize(720, 520))
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* content = new wxBoxSizer(wxHORIZONTAL);

    wxArrayString sections;
    sections.Add("General");
    sections.Add("Scopes");
    sections.Add("Shortcuts");
    sections.Add("About");
    m_sidebar = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(150, -1), sections, wxLB_SINGLE);
    m_sidebar->SetSelection(0);
    m_sidebar->Bind(wxEVT_LISTBOX, &SettingsFrame::onSidebar, this);

    m_book = new wxSimplebook(this, wxID_ANY);
    m_book->AddPage(buildGeneralPage(m_book), "General");
    m_book->AddPage(buildScopesPage(m_book), "Scopes");
    m_shortcutsPage = buildShortcutsPage(m_book);
    m_book->AddPage(m_shortcutsPage, "Shortcuts");
    m_book->AddPage(buildAboutPage(m_book), "About");

    content->Add(m_sidebar, 0, wxEXPAND | wxALL, 8);
    content->Add(m_book, 1, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 8);

    root->Add(content, 1, wxEXPAND);
    root->Add(new wxStaticLine(this), 0, wxEXPAND);

    auto* footer = new wxStaticText(this, wxID_ANY, "SideScopes 0.2.0");
    footer->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    root->Add(footer, 0, wxALL, 8);

    SetSizer(root);
    SetMinSize(wxSize(640, 460));
    Bind(wxEVT_CHAR_HOOK, &SettingsFrame::onCharHook, this);
    Centre();
}

wxStaticText* SettingsFrame::makeHeader(wxWindow* parent, const wxString& text)
{
    auto* header = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = header->GetFont();
    font.MakeBold();
    font.SetPointSize(font.GetPointSize() + 2);
    header->SetFont(font);

    return header;
}

wxPanel* SettingsFrame::buildGeneralPage(wxWindow* parent)
{
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(makeHeader(page, "General"), 0, wxALL, 12);

    auto* graticule = new wxCheckBox(page, wxID_ANY, "Show graticule");
    graticule->SetValue(true);
    sizer->Add(graticule, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* resume = new wxCheckBox(page, wxID_ANY, "Resume capture at launch");
    sizer->Add(resume, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* grid = new wxFlexGridSizer(2, 10, 12);
    grid->AddGrowableCol(1, 1);

    wxArrayString appearances;
    appearances.Add("Light");
    appearances.Add("Dark");
    appearances.Add("System");
    grid->Add(new wxStaticText(page, wxID_ANY, "Appearance"), 0, wxALIGN_CENTER_VERTICAL);
    m_appearance = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, appearances);
    m_appearance->SetSelection(2);
    m_appearance->Bind(wxEVT_CHOICE, &SettingsFrame::onAppearance, this);
    grid->Add(m_appearance, 0, wxEXPAND);

    wxArrayString displays;
    displays.Add("Built-in Display");
    displays.Add("DELL U2720Q");
    displays.Add("Sidecar iPad");
    grid->Add(new wxStaticText(page, wxID_ANY, "Display"), 0, wxALIGN_CENTER_VERTICAL);
    auto* display = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, displays);
    display->SetSelection(0);
    grid->Add(display, 0, wxEXPAND);

    sizer->Add(grid, 0, wxEXPAND | wxALL, 12);

    m_appearanceHint = new wxStaticText(page, wxID_ANY, "");
    m_appearanceHint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    sizer->Add(m_appearanceHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    page->SetSizer(sizer);

    return page;
}

void SettingsFrame::addScopeSection(wxWindow* page, wxSizer* parentSizer, const wxString& title,
                                    const std::vector<std::pair<wxString, std::vector<wxString>>>& choices)
{
    auto* boxSizer = new wxStaticBoxSizer(wxVERTICAL, page, title);
    wxWindow* host = boxSizer->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1, 1);

    const char* sliders[] = {"Intensity", "Detail", "Smoothing"};
    const int defaults[] = {60, 50, 35};
    for (int i = 0; i < 3; ++i) {
        grid->Add(new wxStaticText(host, wxID_ANY, sliders[i]), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxSlider(host, wxID_ANY, defaults[i], 0, 100), 0, wxEXPAND);
    }

    for (const auto& choice : choices) {
        wxArrayString items;
        for (const auto& item : choice.second) {
            items.Add(item);
        }
        grid->Add(new wxStaticText(host, wxID_ANY, choice.first), 0, wxALIGN_CENTER_VERTICAL);
        auto* ctrl = new wxChoice(host, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
        ctrl->SetSelection(0);
        grid->Add(ctrl, 0, wxEXPAND);
    }

    boxSizer->Add(grid, 1, wxEXPAND | wxALL, 8);
    parentSizer->Add(boxSizer, 0, wxEXPAND | wxALL, 10);
}

wxWindow* SettingsFrame::buildScopesPage(wxWindow* parent)
{
    auto* page = new wxScrolledWindow(parent, wxID_ANY);
    page->SetScrollRate(0, 12);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    addScopeSection(page, sizer, "Vectorscope",
                    {{"Matrix", {"BT.601", "BT.709"}}, {"Trace Response", {"Boosted", "Linear"}}});
    addScopeSection(page, sizer, "Waveform", {{"Waveform Style", {"RGB", "Luma", "Colored Luma"}}});
    addScopeSection(page, sizer, "Histogram", {{"Histogram Style", {"Combined", "Per Channel"}}});

    page->SetSizer(sizer);

    return page;
}

wxPanel* SettingsFrame::buildShortcutsPage(wxWindow* parent)
{
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(makeHeader(page, "Shortcuts"), 0, wxALL, 12);

    auto* hint =
        new wxStaticText(page, wxID_ANY, "Double-click a shortcut, then press a letter or digit. Esc cancels.");
    hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    m_list = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->InsertColumn(0, "Action", wxLIST_FORMAT_LEFT, 340);
    m_list->InsertColumn(1, "Key", wxLIST_FORMAT_LEFT, 120);

    m_actions = {"Vectorscope", "Waveform", "RGB Parade", "Histogram", "Color Picker", "Pin", "Zoom", "Pick Window"};
    m_keys = {"V", "W", "R", "H", "C", "P", "Z", "A"};
    for (std::size_t i = 0; i < m_actions.size(); ++i) {
        long row = m_list->InsertItem(static_cast<long>(i), m_actions[i]);
        m_list->SetItem(row, 1, m_keys[i]);
    }
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &SettingsFrame::onRowActivated, this);
    sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* conflictRow = new wxBoxSizer(wxHORIZONTAL);
    m_conflictText = new wxStaticText(page, wxID_ANY, "");
    m_conflictText->SetForegroundColour(*wxRED);
    m_swapButton = new wxButton(page, wxID_ANY, "Swap");
    m_swapButton->Bind(wxEVT_BUTTON, &SettingsFrame::onSwap, this);
    conflictRow->Add(m_conflictText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    conflictRow->Add(m_swapButton, 0);
    sizer->Add(conflictRow, 0, wxEXPAND | wxALL, 12);
    m_conflictText->Hide();
    m_swapButton->Hide();

    page->SetSizer(sizer);

    return page;
}

wxPanel* SettingsFrame::buildAboutPage(wxWindow* parent)
{
    const wxColour muted = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(8);

    auto* title = new wxStaticText(page, wxID_ANY, "SideScopes");
    wxFont font = title->GetFont();
    font.SetPointSize(font.GetPointSize() + 8);
    font.MakeBold();
    title->SetFont(font);
    sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* version = new wxStaticText(page, wxID_ANY, "Version 0.2.0");
    version->SetForegroundColour(muted);
    sizer->Add(version, 0, wxLEFT | wxRIGHT | wxTOP, 6);

    auto* description = new wxStaticText(
        page, wxID_ANY, "Real-time vectorscope, waveform, and histogram\nfor a selected screen region.");
    sizer->Add(description, 0, wxALL, 12);

    auto* license = new wxStaticText(page, wxID_ANY, "GPL-3.0-or-later");
    license->SetForegroundColour(muted);
    sizer->Add(license, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    page->SetSizer(sizer);

    return page;
}

void SettingsFrame::onSidebar(wxCommandEvent& event)
{
    (void)event;
    int selection = m_sidebar->GetSelection();
    if (selection != wxNOT_FOUND) {
        m_book->ChangeSelection(static_cast<std::size_t>(selection));
    }
}

void SettingsFrame::onAppearance(wxCommandEvent& event)
{
    (void)event;
#if wxCHECK_VERSION(3, 3, 0)
    wxApp::Appearance mode = wxApp::Appearance::System;
    switch (m_appearance->GetSelection()) {
    case 0:
        mode = wxApp::Appearance::Light;
        break;
    case 1:
        mode = wxApp::Appearance::Dark;
        break;
    default:
        mode = wxApp::Appearance::System;
        break;
    }

    wxApp::AppearanceResult result = wxGetApp().SetAppearance(mode);
    if (result == wxApp::AppearanceResult::CannotChange) {
        m_appearanceHint->SetLabel("This platform only sets the appearance before the window opens.");
    } else if (result == wxApp::AppearanceResult::Failure) {
        m_appearanceHint->SetLabel("The platform could not change the appearance.");
    } else {
        m_appearanceHint->SetLabel("");
    }
#else
    m_appearanceHint->SetLabel("Live switching needs wxWidgets 3.3+; this build follows the system theme.");
#endif
}

void SettingsFrame::onRowActivated(wxListEvent& event)
{
    startCapture(event.GetIndex());
}

void SettingsFrame::onSwap(wxCommandEvent& event)
{
    (void)event;
    if (m_captureRow < 0 || m_conflictOther < 0) {
        return;
    }

    long row = m_captureRow;
    int other = m_conflictOther;
    setRowKey(other, m_savedKey);
    setRowKey(row, m_pendingKey);
    finishCapture();
}

void SettingsFrame::onCharHook(wxKeyEvent& event)
{
    if (m_captureRow < 0) {
        event.Skip();
        return;
    }

    int code = event.GetKeyCode();
    if (code == WXK_ESCAPE) {
        cancelCapture();
        return;
    }

    bool plain = event.GetModifiers() == wxMOD_NONE;
    bool letterOrDigit = (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9');
    if (plain && letterOrDigit) {
        applyCaptureKey(wxString(static_cast<wxChar>(code)));
        return;
    }

    event.Skip();
}

int SettingsFrame::findKeyOwner(const wxString& key, long exceptRow) const
{
    for (std::size_t i = 0; i < m_keys.size(); ++i) {
        if (static_cast<long>(i) != exceptRow && m_keys[i] == key) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void SettingsFrame::startCapture(long row)
{
    if (m_captureRow >= 0) {
        cancelCapture();
    }

    m_captureRow = row;
    m_savedKey = m_keys[static_cast<std::size_t>(row)];
    m_list->SetItem(row, 1, "press a key...");
    hideConflict();
}

void SettingsFrame::applyCaptureKey(const wxString& newKey)
{
    int other = findKeyOwner(newKey, m_captureRow);
    if (other < 0) {
        setRowKey(m_captureRow, newKey);
        finishCapture();
        return;
    }

    m_conflictOther = other;
    m_pendingKey = newKey;
    m_list->SetItem(m_captureRow, 1, newKey);
    showConflict(newKey, m_actions[static_cast<std::size_t>(other)]);
}

void SettingsFrame::setRowKey(long row, const wxString& key)
{
    m_keys[static_cast<std::size_t>(row)] = key;
    m_list->SetItem(row, 1, key);
}

void SettingsFrame::showConflict(const wxString& key, const wxString& otherAction)
{
    m_conflictText->SetLabel(wxString::Format("\"%s\" is already used by %s.", key, otherAction));
    m_conflictText->Show();
    m_swapButton->Show();
    m_shortcutsPage->Layout();
}

void SettingsFrame::hideConflict()
{
    m_conflictText->Hide();
    m_swapButton->Hide();
    m_shortcutsPage->Layout();
}

void SettingsFrame::finishCapture()
{
    m_captureRow = -1;
    m_conflictOther = -1;
    m_pendingKey.clear();
    hideConflict();
}

void SettingsFrame::cancelCapture()
{
    if (m_captureRow >= 0) {
        m_list->SetItem(m_captureRow, 1, m_savedKey);
    }

    finishCapture();
}

bool SpikeApp::OnInit()
{
    auto* frame = new SettingsFrame();
    frame->Show(true);
    frame->Raise();

    return true;
}

wxIMPLEMENT_APP(SpikeApp);
