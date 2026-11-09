#ifndef EPS16_VST3_PANEL_EDITOR_H
#define EPS16_VST3_PANEL_EDITOR_H

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class Eps16PanelEditor final : public juce::AudioProcessorEditor,
                               private juce::Timer {
public:
    explicit Eps16PanelEditor(Eps16PlusProcessor &);
    ~Eps16PanelEditor() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(FocusChangeType) override;

private:
    class EpsFaderLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        void drawLinearSlider(juce::Graphics &, int x, int y, int width,
                              int height, float sliderPosition,
                              float minimumSliderPosition,
                              float maximumSliderPosition,
                              juce::Slider::SliderStyle,
                              juce::Slider &) override;
        juce::Image thumbImage;
    };

    class VfdLabel final : public juce::Label {
    public:
        void setCursorSegmentMask(std::uint32_t mask);
        void setDecimalMask(std::uint32_t mask);
        void setIndicators(const std::array<std::uint16_t, 3> &on,
                           const std::array<std::uint16_t, 3> &flash,
                           bool flashPhase);
        void paint(juce::Graphics &) override;

    private:
        std::uint32_t cursorSegmentMask{};
        std::uint32_t decimalMask{};
        std::array<std::uint16_t, 3> indicatorOn{};
        std::array<std::uint16_t, 3> indicatorFlash{};
        bool indicatorFlashPhase{};
    };

    class PanelButton final : public juce::TextButton {
    public:
        enum class Style { SmallBlack, SmallGrey, LargeBlack };

        PanelButton(Eps16PlusProcessor &, juce::String label,
                    std::uint8_t rawCode, bool mappingKnown = true);
        void setShiftChordCode(std::uint8_t rawCode);
        void setSprite(const juce::Image &sheet, Style style);
        void triggerShortcut();
        void showActivationGlow();
        void updateActivationGlow(double nowMs);
        void paintButton(juce::Graphics &, bool highlighted,
                         bool down) override;
        void mouseDown(const juce::MouseEvent &) override;
        void mouseUp(const juce::MouseEvent &) override;
        void mouseExit(const juce::MouseEvent &) override;

    private:
        void startActivationGlow();
        void releaseIfNeeded();
        Eps16PlusProcessor &processor;
        const std::uint8_t code;
        std::uint8_t shiftChordCode{0xff};
        bool pressed{};
        bool shiftChordPressed{};
        double activationGlowStartedMs{-1.0};
        juce::Image spriteSheet;
        Style buttonStyle{Style::SmallBlack};
    };

    class DiskButton final : public juce::Button {
    public:
        DiskButton(juce::String name, juce::String diskLabel);
        void paintButton(juce::Graphics &, bool highlighted,
                         bool down) override;

    private:
        juce::String label;
    };

public:
    /* PSEL is one analog line shared by both physical buttons (no panel
       scan code - never route this through addPanelButton()). Held state
       has two independent sources: a plain click/hold is momentary, an
       Option(Alt)-click toggles a latch that holds with no mouse button
       down; a plain click on an already-latched button clears the latch
       rather than starting a new momentary press. effectiveHeld() is
       their OR, and a latched button paints in its pressed state so the
       latch stays visible. See
       Eps16PanelEditor::updatePatchSelectAnalog() for how the editor
       combines both buttons' effectiveHeld() into the one analog write.
       effectiveHeld() is also mirrored through the inherited
       getToggleState().

       press()/release() carry the whole state transition; the mouse
       handlers are thin wrappers over them. juce::Button's handlers are
       protected, so a test cannot drive this through them - it calls
       press()/release() instead, which is why the class is public. */
    class PatchSelectButton final : public juce::Button {
    public:
        explicit PatchSelectButton(bool pointsRight);
        [[nodiscard]] bool effectiveHeld() const {
            return latched || heldByMouse;
        }

        /* altDown toggles the latch; a plain press on a latched button
           clears it; otherwise the press is momentary. */
        void press(bool altDown);
        void release();

        /* Two-frame sheet, same convention as PanelButton::setSprite():
           released on top, pressed below, frame height = sheet height / 2. */
        void setSprite(const juce::Image &sheet);

        void paintButton(juce::Graphics &, bool highlighted,
                         bool down) override;
        void mouseDown(const juce::MouseEvent &) override;
        void mouseUp(const juce::MouseEvent &) override;
        void mouseExit(const juce::MouseEvent &) override;

        std::function<void()> onEffectiveHeldChanged;

    private:
        void notifyChanged();

        juce::Image spriteSheet;
        bool latched{};
        bool heldByMouse{};
    };

private:
    class PianoKeyboard final : public juce::Component {
    public:
        explicit PianoKeyboard(Eps16PlusProcessor &);
        ~PianoKeyboard() override;
        void paint(juce::Graphics &) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent &) override;
        void mouseDrag(const juce::MouseEvent &) override;
        void mouseUp(const juce::MouseEvent &) override;
        void mouseExit(const juce::MouseEvent &) override;
        void releaseAllNotes();
        void releasePerformanceControls();

    private:
        class PerformanceWheel final : public juce::Slider {
        public:
            void paint(juce::Graphics &) override;
        };

        static bool isBlackKey(int note);
        static std::uint16_t wheelToAnalog(double value);
        juce::Rectangle<float> keyboardArea() const;
        juce::Rectangle<float> keyBounds(int note) const;
        int noteAt(juce::Point<float> position) const;
        std::uint8_t velocityAt(int note, float y) const;
        void pressAt(juce::Point<float> position);
        void releaseActiveNote();

        Eps16PlusProcessor &processor;
        PerformanceWheel pitchWheel;
        PerformanceWheel modWheel;
        int activeNote{-1};
    };

    PanelButton &addPanelButton(const juce::String &, std::uint8_t,
                                bool known = true);
    bool updateArrowKey(int keyCode, bool isDown);
    void releaseArrowKeys();
    void openSaveDiskDialog(bool hfeFormat);
    void updateDiskName();
    void setKeyboardExpanded(bool expanded);
    void timerCallback() override;
    void loadPanelImages();
    void loadLayoutOverrides();
    juce::Rectangle<int> designRect(const juce::String &name, int x, int y,
                                    int width, int height) const;
    void updatePatchSelectAnalog();

    Eps16PlusProcessor &owner;
    VfdLabel vfd;
    juce::Label status;
    EpsFaderLookAndFeel faderLookAndFeel;
    juce::Image panelBackground;
    juce::Image spriteSmallBlack;
    juce::Image spriteSmallGrey;
    juce::Image spriteLargeBlack;
    juce::Image spriteLargeGrey;
    juce::Image sliderThumb;
    juce::Image trackLedSprite;
    std::map<juce::String, juce::Rectangle<int>> layoutOverrides;
    juce::Slider masterVolume;
    juce::Slider dataEntry;
    DiskButton osDiskButton{"Insert OS disk", "OS"};
    DiskButton newDiskButton{"New blank disk", "NEW"};
    DiskButton loadDiskButton{"Load disk image", "LOAD"};
    DiskButton saveDiskButton{"Save disk image", "SAVE"};
    juce::Label diskName;
    juce::TextButton keyboardToggle{"KEYBOARD"};
    PianoKeyboard pianoKeyboard;
    unsigned int timerTicks{};
    bool keyboardExpanded{};
    std::unique_ptr<juce::FileChooser> diskChooser;
    std::vector<std::unique_ptr<PanelButton>> buttons;
    std::array<PanelButton *, 12> pageButtons{};
    std::array<PanelButton *, 7> modeButtons{};
    std::array<PanelButton *, 8> trackButtons{};
    std::uint16_t trackLedOn{};
    std::uint16_t trackLedFlash{};
    bool trackLedFlashPhase{};
    std::array<juce::Rectangle<int>, 8> trackLedBounds{};
    std::array<PanelButton *, 3> sequencerButtons{};
    PanelButton *upButton{};
    PanelButton *downButton{};
    PanelButton *leftButton{};
    PanelButton *rightButton{};
    PanelButton *cancelButton{};
    PanelButton *enterButton{};
    std::array<bool, 4> arrowKeysDown{};
    PatchSelectButton patchSelectLeft{false};
    PatchSelectButton patchSelectRight{true};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Eps16PanelEditor)
};

#endif
