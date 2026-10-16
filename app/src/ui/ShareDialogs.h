#pragma once

// The Share menu's dialogs (spec 04 §9.2, 05 §3): Sign In, Share (invite links, members
// and roles) and Join from an invite link. Like the project dialogs, the controls are
// public so the [UI] tests drive them the way the user would. Relay calls block the
// message thread for at most their timeout (D-COL-14).

#include "ProjectDialogs.h"

namespace weft::ui
{
/** Share › Sign In (04 §12): an account on a relay, or a new one. */
class SignInDialog final : public juce::Component
{
public:
    explicit SignInDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::TextEditor relay, email, password, displayName;
    juce::ToggleButton createAccount { "Create a new account" };
    juce::TextButton signIn { "Sign In" }, cancel { "Cancel" };
    juce::Label error;
    std::function<void()> onClose;
    /** Called once signed in (the Share or Join step that asked for it continues). */
    std::function<void()> onSignedIn;

    bool submit();

private:
    void updateMode();
    Studio& studio;
};

/** Share › Share Project / Invite (04 §9.2): uploads an unshared project, then shows the
    invite link with its role and the members (the Owner can change roles and remove). */
class ShareDialog final : public juce::Component
{
public:
    explicit ShareDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    // Before sharing.
    juce::Label uploadNote;
    juce::TextButton proceed { "Continue" };
    // Once shared.
    juce::TextEditor link;
    juce::ComboBox role; // 1 Editor, 2 Viewer
    juce::TextButton createLink { "Create Link" }, copy { "Copy" };
    juce::Label removalNote;
    struct MemberRow final : public juce::Component
    {
        MemberRow();
        void resized() override;
        collab::MemberInfo member;
        juce::Label name;
        juce::ComboBox role; // 1 Owner, 2 Editor, 3 Viewer
        juce::TextButton remove { "Remove" };
    };
    std::vector<std::unique_ptr<MemberRow>> members;
    juce::TextButton close { "Close" };
    juce::Label error;
    std::function<void()> onClose;

    bool share();
    bool makeLink();
    bool refreshMembers();
    bool changeRole (const std::string& userId, const std::string& newRole);
    bool removeMember (const std::string& userId);

private:
    void layoutMode();
    std::string myRole() const;
    Studio& studio;
};

/** Share › Join from Invite Link: accepts the invite and opens the project (05 §3.2). */
class JoinDialog final : public juce::Component
{
public:
    explicit JoinDialog (Studio& studio);
    void resized() override;
    void paint (juce::Graphics&) override;

    juce::TextEditor link;
    juce::TextButton join { "Join" }, cancel { "Cancel" };
    juce::Label error;
    std::function<void()> onClose;

    bool submit();

private:
    Studio& studio;
};
} // namespace weft::ui
