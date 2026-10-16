#include "ShareDialogs.h"

namespace weft::ui
{
namespace
{
constexpr int labelX = 20, controlX = 150, rowHeight = 34;

void drawLabels (juce::Graphics& g, const juce::StringArray& labels)
{
    g.fillAll (colour (Token::surfaceRaised));
    g.setColour (colour (Token::text));
    g.setFont (juce::FontOptions (13.0f));
    for (int i = 0; i < labels.size(); ++i)
        if (labels[i].isNotEmpty())
            g.drawText (labels[i], labelX, 16 + i * rowHeight, controlX - labelX - 8, 26, juce::Justification::centredLeft);
}

void styleError (juce::Label& l) { l.setColour (juce::Label::textColourId, colour (Token::warning)); }

juce::String text (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
std::string utf8 (const juce::String& s) { return s.trim().toStdString(); }

void closeWith (const std::function<void()>& onClose)
{
    if (onClose)
        onClose();
}
} // namespace

// Sign In -------------------------------------------------------------------------------------

SignInDialog::SignInDialog (Studio& s) : studio (s)
{
    relay.setText (text (studio.account().relayUrl), false);
    relay.setTextToShowWhenEmpty ("https://your-relay.example", colour (Token::textDim));
    email.setText (text (studio.account().email), false);
    password.setPasswordCharacter (static_cast<juce::juce_wchar> (0x2022));
    displayName.setText (text (studio.userDisplayName), false);
    describe (relay, "Relay", "The address of the relay that hosts your shared projects");
    describe (email, "Email");
    describe (password, "Password", "At least 8 characters");
    describe (displayName, "Your name", "Shown to collaborators");
    describe (createAccount, "Create a new account");
    describe (signIn, "Sign In", "Sign in (Enter)");
    describe (cancel, "Cancel", "Close (Esc)");
    signIn.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    createAccount.onClick = [this] { updateMode(); };
    signIn.onClick = [this] {
        if (submit())
        {
            auto next = onSignedIn;
            closeWith (onClose);
            if (next)
                next();
        }
    };
    cancel.onClick = [this] { closeWith (onClose); };
    styleError (error);
    for (auto* c : std::initializer_list<juce::Component*> { &relay, &email, &password, &createAccount, &signIn, &cancel, &error })
        addAndMakeVisible (c);
    addChildComponent (displayName);
    setSize (460, 270);
    updateMode();
}

void SignInDialog::updateMode()
{
    displayName.setVisible (createAccount.getToggleState());
    signIn.setButtonText (createAccount.getToggleState() ? "Create Account" : "Sign In");
    repaint();
}

bool SignInDialog::submit()
{
    const auto url = collab::normalizeRelayUrl (utf8 (relay.getText()));
    if (url.empty())
    {
        error.setText ("Enter the relay's address.", juce::dontSendNotification);
        return false;
    }
    std::string problem;
    const auto account = createAccount.getToggleState()
                             ? collab::RelayApi::signUp (url, utf8 (email.getText()), password.getText().toStdString(), utf8 (displayName.getText()), &problem)
                             : collab::RelayApi::signIn (url, utf8 (email.getText()), password.getText().toStdString(), &problem);
    if (! account)
    {
        error.setText (text (problem), juce::dontSendNotification);
        return false;
    }
    studio.signedInAs (*account);
    studio.setStatus ("Signed in as " + text (account->displayName) + ".");
    return true;
}

void SignInDialog::paint (juce::Graphics& g)
{
    drawLabels (g, { "Relay", "Email", "Password", "", createAccount.getToggleState() ? "Your name" : "" });
}

void SignInDialog::resized()
{
    const auto w = getWidth() - controlX - 20;
    relay.setBounds (controlX, 16, w, 26);
    email.setBounds (controlX, 16 + rowHeight, w, 26);
    password.setBounds (controlX, 16 + 2 * rowHeight, w, 26);
    createAccount.setBounds (controlX, 16 + 3 * rowHeight, w, 26);
    displayName.setBounds (controlX, 16 + 4 * rowHeight, w, 26);
    error.setBounds (labelX, 16 + 5 * rowHeight, getWidth() - 40, 22);
    signIn.setBounds (getWidth() - 240, getHeight() - 42, 124, 28);
    cancel.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Share ---------------------------------------------------------------------------------------

ShareDialog::MemberRow::MemberRow()
{
    role.addItem ("Owner", 1);
    role.addItem ("Editor", 2);
    role.addItem ("Viewer", 3);
    role.setItemEnabled (1, false);
    describe (role, "Role");
    describe (remove, "Remove", "Remove this member from the project");
    for (auto* c : std::initializer_list<juce::Component*> { &name, &role, &remove })
        addAndMakeVisible (c);
}

void ShareDialog::MemberRow::resized()
{
    auto r = getLocalBounds();
    remove.setBounds (r.removeFromRight (80).reduced (0, 2));
    r.removeFromRight (8);
    role.setBounds (r.removeFromRight (110).reduced (0, 2));
    name.setBounds (r);
}

ShareDialog::ShareDialog (Studio& s) : studio (s)
{
    uploadNote.setJustificationType (juce::Justification::topLeft);
    role.addItem ("Editor", 1);
    role.addItem ("Viewer", 2);
    role.setSelectedId (1, juce::dontSendNotification);
    link.setReadOnly (true);
    link.setTextToShowWhenEmpty ("Create a link to invite someone", colour (Token::textDim));
    describe (link, "Invite link", "Anyone with this link and an account on the relay can join for 7 days");
    describe (role, "Invite as", "Editors can change the project; Viewers can comment and chat");
    describe (createLink, "Create Link", "Create an invite link for this role");
    describe (copy, "Copy", "Copy the invite link");
    describe (proceed, "Continue", "Upload the project to the relay");
    describe (close, "Close", "Close (Esc)");
    removalNote.setText ("Removing someone ends their access. A copy already on their computer stays there.", juce::dontSendNotification);
    removalNote.setColour (juce::Label::textColourId, colour (Token::textDim));
    removalNote.setFont (juce::FontOptions (12.0f));
    close.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    proceed.onClick = [this] { share(); };
    createLink.onClick = [this] { makeLink(); };
    copy.onClick = [this] {
        if (link.getText().isNotEmpty())
            juce::SystemClipboard::copyTextToClipboard (link.getText());
    };
    close.onClick = [this] { closeWith (onClose); };
    styleError (error);
    for (auto* c : std::initializer_list<juce::Component*> { &uploadNote, &proceed, &link, &role, &createLink, &copy, &removalNote, &close, &error })
        addChildComponent (c);
    close.setVisible (true);
    error.setVisible (true);
    setSize (560, 420);
    layoutMode();
}

std::string ShareDialog::myRole() const
{
    if (const auto* sync = studio.sync(); sync != nullptr && ! sync->role().empty())
        return sync->role();
    for (const auto& m : members)
        if (m->member.userId == studio.account().userId)
            return m->member.role;
    return {};
}

void ShareDialog::layoutMode()
{
    const bool shared = studio.shared();
    uploadNote.setVisible (! shared);
    proceed.setVisible (! shared);
    for (auto* c : std::initializer_list<juce::Component*> { &link, &role, &createLink, &copy, &removalNote })
        c->setVisible (shared);
    if (! shared)
        uploadNote.setText ("This uploads the project to " + text (studio.account().relayUrl)
                                + " so you can invite others to it. Edits sync live from then on. Audio files stay on this computer for now: "
                                  "media sync arrives in a later version.",
                            juce::dontSendNotification);
    else
        refreshMembers();
    resized();
    repaint();
}

bool ShareDialog::share()
{
    std::string problem;
    if (! studio.shareProject (&problem))
    {
        error.setText (text (problem), juce::dontSendNotification);
        return false;
    }
    error.setText ({}, juce::dontSendNotification);
    layoutMode();
    return true;
}

bool ShareDialog::makeLink()
{
    const std::string asRole = role.getSelectedId() == 2 ? "viewer" : "editor";
    std::string problem;
    const auto invite = studio.withRelay ([&] (collab::RelayApi& api) {
        auto r = api.createInvite (studio.model().info.id, asRole);
        if (! r)
            problem = api.error();
        return r;
    });
    if (! invite)
    {
        error.setText (text (problem), juce::dontSendNotification);
        return false;
    }
    link.setText (text (invite->url), false);
    error.setText ({}, juce::dontSendNotification);
    return true;
}

bool ShareDialog::refreshMembers()
{
    std::string problem;
    const auto list = studio.withRelay ([&] (collab::RelayApi& api) {
        auto r = api.members (studio.model().info.id);
        if (r.empty())
            problem = api.error();
        return r;
    });
    members.clear();
    if (list.empty())
    {
        error.setText (text (problem.empty() ? std::string ("Couldn't load the members.") : problem), juce::dontSendNotification);
        return false;
    }
    const auto me = studio.account().userId;
    std::string mine;
    for (const auto& m : list)
        if (m.userId == me)
            mine = m.role;
    for (const auto& m : list)
    {
        auto row = std::make_unique<MemberRow>();
        row->member = m;
        row->name.setText (text (m.displayName) + (m.userId == me ? " (you)" : "") + "   " + text (m.email), juce::dontSendNotification);
        row->role.setSelectedId (m.role == "owner" ? 1 : m.role == "editor" ? 2 : 3, juce::dontSendNotification);
        const bool manage = mine == "owner" && m.role != "owner";
        row->role.setEnabled (manage);
        row->remove.setVisible (manage);
        const auto userId = m.userId;
        auto* r = row.get();
        row->role.onChange = [this, userId, r] { changeRole (userId, r->role.getSelectedId() == 2 ? "editor" : "viewer"); };
        row->remove.onClick = [this, userId] { removeMember (userId); };
        addAndMakeVisible (*row);
        members.push_back (std::move (row));
    }
    // Viewers can't invite (05 §3.2).
    createLink.setEnabled (mine != "viewer");
    role.setEnabled (mine != "viewer");
    error.setText ({}, juce::dontSendNotification);
    resized();
    return true;
}

bool ShareDialog::changeRole (const std::string& userId, const std::string& newRole)
{
    std::string problem;
    const bool ok = studio.withRelay ([&] (collab::RelayApi& api) {
        const bool r = api.setRole (studio.model().info.id, userId, newRole);
        if (! r)
            problem = api.error();
        return r;
    });
    if (! ok)
        error.setText (text (problem), juce::dontSendNotification);
    refreshMembers();
    return ok;
}

bool ShareDialog::removeMember (const std::string& userId)
{
    std::string problem;
    const bool ok = studio.withRelay ([&] (collab::RelayApi& api) {
        const bool r = api.removeMember (studio.model().info.id, userId);
        if (! r)
            problem = api.error();
        return r;
    });
    juce::Component::SafePointer<ShareDialog> safe (this);
    // The row that was clicked is deleted by the refresh, so do it after this click returns.
    juce::MessageManager::callAsync ([safe, ok, problem] {
        if (safe == nullptr)
            return;
        safe->refreshMembers();
        if (! ok)
            safe->error.setText (text (problem), juce::dontSendNotification);
    });
    return ok;
}

void ShareDialog::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::surfaceRaised));
    if (! studio.shared())
        return;
    g.setColour (colour (Token::text));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Invite link", labelX, 16, controlX - labelX, 26, juce::Justification::centredLeft);
    g.drawText ("Invite as", labelX, 16 + rowHeight, controlX - labelX, 26, juce::Justification::centredLeft);
    g.drawText ("Members", labelX, 16 + 2 * rowHeight + 8, controlX - labelX, 26, juce::Justification::centredLeft);
}

void ShareDialog::resized()
{
    const auto w = getWidth() - controlX - 20;
    uploadNote.setBounds (labelX, 16, getWidth() - 40, 90);
    link.setBounds (controlX, 16, w - 70, 26);
    copy.setBounds (getWidth() - 84, 16, 64, 26);
    role.setBounds (controlX, 16 + rowHeight, 120, 26);
    createLink.setBounds (controlX + 130, 16 + rowHeight, 110, 26);
    int y = 16 + 3 * rowHeight;
    for (auto& m : members)
    {
        m->setBounds (labelX, y, getWidth() - 40, 28);
        y += 32;
    }
    removalNote.setBounds (labelX, getHeight() - 96, getWidth() - 40, 20);
    error.setBounds (labelX, getHeight() - 72, getWidth() - 40, 22);
    proceed.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    close.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}

// Join ----------------------------------------------------------------------------------------

JoinDialog::JoinDialog (Studio& s) : studio (s)
{
    link.setTextToShowWhenEmpty ("Paste the invite link", colour (Token::textDim));
    describe (link, "Invite link");
    describe (join, "Join", "Join the project (Enter)");
    describe (cancel, "Cancel", "Close (Esc)");
    join.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
    cancel.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
    join.onClick = [this] {
        if (submit())
            closeWith (onClose);
    };
    cancel.onClick = [this] { closeWith (onClose); };
    styleError (error);
    for (auto* c : std::initializer_list<juce::Component*> { &link, &join, &cancel, &error })
        addAndMakeVisible (c);
    setSize (520, 140);
}

bool JoinDialog::submit()
{
    std::string problem;
    if (! studio.joinProject (utf8 (link.getText()), &problem))
    {
        error.setText (text (problem), juce::dontSendNotification);
        return false;
    }
    return true;
}

void JoinDialog::paint (juce::Graphics& g) { drawLabels (g, { "Invite link" }); }

void JoinDialog::resized()
{
    link.setBounds (controlX, 16, getWidth() - controlX - 20, 26);
    error.setBounds (labelX, 16 + rowHeight, getWidth() - 40, 22);
    join.setBounds (getWidth() - 200, getHeight() - 42, 84, 28);
    cancel.setBounds (getWidth() - 104, getHeight() - 42, 84, 28);
}
} // namespace weft::ui
