// The launcher window. See launcher.h. Adapted from the launcher window of the Prototype Mac port
// (runtime/loader/rt_launcher.m there): the same layout, controls and behavior, with the settings, the game folder
// check and the look of this game.
//
// The look is that of the game's 1920s Innsmouth: a dark sea-green ground, bone-white text, tarnished brass for the
// group titles and the rule, verdigris for Play, and Baskerville (a font of macOS) for the title. The controls are
// standard AppKit controls in the dark appearance:
//   - left: the graphics settings, and the sound and mouse settings,
//   - right, top: the game folder and the game settings,
//   - right, bottom: Restore Defaults and Play (Return), level with the bottom of the left column.
// When the selected folder can be used, the header shows the icon of the GOG release from that folder
// (goggame-1189711155.ico). The repository and the app hold no game art.
//
// The settings file is Enhancements.cfg (enhance.h). A file that cannot be read, or that has lines that are not
// valid settings, is not changed until the player selects Play; the window tells the player about the problem.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "enhance.h"
#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAUNCHER_PATH_LEN 1024
#define MAX_VALUES 8
#define GAME_ICON "goggame-1189711155.ico"           // the icon of the GOG release, in the game folder
static NSString *const DEFAULTS_GAME_DIR = @"GameFolder";   // the folder of the last Play (NSUserDefaults)

// The text of a setting in the window: the field of the setting in RtEnh, and one name for each value 0 to the
// maximum of the setting (coc_enh_keys).
typedef struct setting_text {
    size_t off;
    const char *label;
    const char *hint;
    const char *values[MAX_VALUES];
} setting_text;

static const setting_text graphics_rows[] = {
    { offsetof(RtEnh, aa), "Anti-Aliasing (MetalFX)", "Smooths the edges of the 3D scene.", { "Off", "On" } },
    { offsetof(RtEnh, ao), "Ambient Occlusion", "Darker corners and contact areas.", { "Off", "Low", "Medium", "High" } },
    { offsetof(RtEnh, contact), "Contact Shadows", "Small shadows where a character blocks the light.", { "Off", "On" } },
    { offsetof(RtEnh, shadows), "Sharp Character Shadows", "Sharper shadows. Uses more graphics memory.", { "Off", "On" } },
    { offsetof(RtEnh, bloom), "Bloom", "A soft glow that spreads from bright areas.", { "Off", "Low", "Medium", "High" } },
    { offsetof(RtEnh, tonemap), "GT7 Tone Mapping", "Keeps the detail of lamps and fire.", { "Off", "On" } },
    { offsetof(RtEnh, hdr), "HDR Output", "For an HDR display. Calibrate in Options > HDR Calibration.", { "Off", "On" } },
};

static const setting_text sound_rows[] = {
    { offsetof(RtEnh, hrtf), "Headphone Audio (HRTF)", "3D sound through the spatial audio of macOS.", { "Off", "On" } },
    { offsetof(RtEnh, raw_mouse), "Raw Mouse Input", "Mouse movement without the acceleration of macOS.", { "Off", "On" } },
};

static const setting_text game_rows[] = {
    { offsetof(RtEnh, fov), "Field of View", "A wider view. Cut scenes keep theirs.", { "Game", "110%", "120%", "130%", "140%" } },
    { offsetof(RtEnh, sanity), "Sanity Effects", "The strength of the visual effects of fear.",
      { "Game", "75%", "50%", "25%", "Off" } },
    { offsetof(RtEnh, sub_scale), "Subtitle Size", "The size of the subtitle text.", { "Game", "1.25x", "1.5x", "1.75x", "2x" } },
    { offsetof(RtEnh, skip_intro), "Skip Intro Videos", "The game starts at the main menu.", { "Off", "On" } },
};

#define NGRAPHICS (sizeof graphics_rows / sizeof graphics_rows[0])
#define NSOUND (sizeof sound_rows / sizeof sound_rows[0])
#define NGAMEROWS (sizeof game_rows / sizeof game_rows[0])

// ---------------------------------------------------------------- palette

static NSColor *rgb(unsigned hex)
{
    return [NSColor colorWithSRGBRed:(CGFloat)((hex >> 16) & 0xFFu) / 255.0
                               green:(CGFloat)((hex >> 8) & 0xFFu) / 255.0
                                blue:(CGFloat)(hex & 0xFFu) / 255.0
                               alpha:1.0];
}

#define C_GROUND_TOP 0x101816u  // window, top: deep sea green
#define C_GROUND 0x0B100Fu      // window, bottom
#define C_PANEL 0x161D1Bu       // setting groups
#define C_TEXT 0xE6DFCCu        // labels: bone white (12.9:1 on the panel)
#define C_SUBTITLE 0xB8AF98u    // the second title line (8.8:1 on the window)
#define C_HINT 0xA39C89u        // hints (6.3:1 on the panel)
#define C_BRASS 0xC9A764u       // group titles and the rule: tarnished brass (7.5:1 on the panel, 8.4:1 on the window)
#define C_VERDIGRIS 0x2F6B5Eu   // the Play button (white text 6.2:1)
#define C_OK 0x7DBF8Eu
#define C_WARN 0xE0A84Au

// ---------------------------------------------------------------- errors

static _Noreturn void launcher_fail(NSString *title, NSString *text)
{
    NSAlert *a = [NSAlert new];
    fprintf(stderr, "Call of Cthulhu: %s %s\n", title.UTF8String, text.UTF8String);
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = title;
    a.informativeText = text;
    [a runModal];
    exit(1);
}

static NSString *nsstr(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    return r != nil ? r : @"(a name that is not UTF-8)";
}

static NSFont *font_named(NSString *name, CGFloat size)
{
    NSFont *f = [NSFont fontWithName:name size:size];
    if (f == nil) launcher_fail(@"A font of macOS is missing.", [NSString stringWithFormat:@"The font %@ is not installed.", name]);
    return f;
}

// The index in coc_enh_keys of the setting at offset off of RtEnh.
static size_t key_index(size_t off)
{
    size_t i;
    for (i = 0; i < coc_enh_nkeys; i++)
        if (coc_enh_keys[i].off == off) return i;
    launcher_fail(@"The launcher is damaged.", @"A setting of the window is not in the settings file.");
}

static int32_t *setting_field(RtEnh *e, size_t off)
{
    return (int32_t *)(void *)((char *)e + off);
}

// ---------------------------------------------------------------- window

@interface CoCLauncher : NSObject <NSWindowDelegate>
@property(nonatomic) NSWindow *window;
@property(nonatomic) NSTextField *path;
@property(nonatomic) NSImageView *statusIcon;
@property(nonatomic) NSTextField *status;
@property(nonatomic) NSTextField *banner;
@property(nonatomic) NSButton *play;
@property(nonatomic) NSMutableDictionary<NSNumber *, NSPopUpButton *> *popups;   // key: the offset of the setting
@property(nonatomic) NSImageView *icon;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL playChosen;
@end

@implementation CoCLauncher
{
@public
    char dir[LAUNCHER_PATH_LEN];
    bool dirOK;
    coc_launcher_check_fn check;
    char iconDir[LAUNCHER_PATH_LEN]; // the folder of the shown icon, or ""
    RtEnh values;                    // the settings of the file; the window changes the ones it shows
}

static NSTextField *text_label(NSString *s, NSFont *font, unsigned color)
{
    NSTextField *t = [NSTextField labelWithString:s];
    t.font = font;
    t.textColor = rgb(color);
    return t;
}

static NSTextField *spaced_label(NSString *s, NSFont *font, unsigned color, CGFloat kern)
{
    NSTextField *t = [NSTextField labelWithAttributedString:[[NSAttributedString alloc]
                                                                initWithString:s
                                                                    attributes:@{
                                                                        NSFontAttributeName : font,
                                                                        NSForegroundColorAttributeName : rgb(color),
                                                                        NSKernAttributeName : @(kern),
                                                                    }]];
    t.accessibilityRole = NSAccessibilityStaticTextRole;
    return t;
}

static NSTextField *group_title(NSString *s)
{
    return spaced_label(s.uppercaseString, font_named(@"Baskerville-SemiBold", 15), C_BRASS, 1.6);
}

#define PANEL_PAD 20 // the left and right inset of a panel (panel())
#define MENU_WIDTH 118
#define GRID_GAP 16
#define LEFT_WIDTH 480
#define RIGHT_WIDTH 330

// A setting group for a panel of the given width: the title, then one row for each setting (the label and its hint
// at the left, the menu at the right).
- (NSStackView *)groupWithTitle:(NSString *)title
                           rows:(const setting_text *)rows
                          count:(size_t)count
                     panelWidth:(CGFloat)width
{
    NSMutableArray<NSArray<NSView *> *> *cells = [NSMutableArray new];
    NSGridView *grid;
    NSStackView *stack;
    size_t i;
    for (i = 0; i < count; i++)
    {
        const setting_text *st = &rows[i];
        const CocEnhKey *k = &coc_enh_keys[key_index(st->off)];
        NSTextField *label = text_label(nsstr(st->label), [NSFont systemFontOfSize:13 weight:NSFontWeightMedium], C_TEXT);
        NSTextField *hint = text_label(nsstr(st->hint), [NSFont systemFontOfSize:11], C_HINT);
        NSStackView *texts = [NSStackView stackViewWithViews:@[ label, hint ]];
        NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        int32_t v;
        texts.orientation = NSUserInterfaceLayoutOrientationVertical;
        texts.alignment = NSLayoutAttributeLeading;
        texts.spacing = 1;
        hint.lineBreakMode = NSLineBreakByWordWrapping;
        hint.preferredMaxLayoutWidth = width - 2 * PANEL_PAD - MENU_WIDTH - GRID_GAP;
        if (k->max + 1 > MAX_VALUES) launcher_fail(@"The launcher is damaged.", @"A setting has too many values.");
        for (v = 0; v <= k->max; v++)
        {
            if (st->values[v] == NULL) launcher_fail(@"The launcher is damaged.", @"A setting value has no name.");
            [pop addItemWithTitle:nsstr(st->values[v])];
        }
        if (k->max + 1 < MAX_VALUES && st->values[k->max + 1] != NULL)
            launcher_fail(@"The launcher is damaged.", @"A setting has more value names than values.");
        pop.accessibilityLabel = nsstr(st->label);
        pop.toolTip = nsstr(st->hint);
        [pop.widthAnchor constraintEqualToConstant:MENU_WIDTH].active = YES;
        self.popups[@(st->off)] = pop;
        [cells addObject:@[ texts, pop ]];
    }
    grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing = 12;
    grid.columnSpacing = GRID_GAP;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementTrailing;
    grid.rowAlignment = NSGridRowAlignmentNone;
    for (i = 0; i < count; i++) [grid rowAtIndex:(NSInteger)i].yPlacement = NSGridCellPlacementCenter;
    stack = [NSStackView stackViewWithViews:@[ group_title(title), grid ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    [grid.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}

// A panel hugs its content, and the content wants the smallest height that its views allow (a weak height-0
// constraint: weaker than the compression resistance of the views, 750, stronger than the hugging of the columns,
// 250). So the free space of a column goes between its panels, not into the rows of a panel.
static NSView *panel(NSStackView *content, CGFloat width)
{
    NSView *p = [NSView new];
    NSLayoutConstraint *hug, *shortest;
    p.wantsLayer = YES;
    p.layer.backgroundColor = [rgb(C_PANEL) colorWithAlphaComponent:0.92].CGColor;
    p.layer.cornerRadius = 12;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [p addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:p.leadingAnchor constant:PANEL_PAD],
        [content.trailingAnchor constraintEqualToAnchor:p.trailingAnchor constant:-PANEL_PAD],
        [content.topAnchor constraintEqualToAnchor:p.topAnchor constant:18],
        [content.bottomAnchor constraintLessThanOrEqualToAnchor:p.bottomAnchor constant:-20],
        [p.widthAnchor constraintEqualToConstant:width],
    ]];
    hug = [content.bottomAnchor constraintEqualToAnchor:p.bottomAnchor constant:-20];
    hug.priority = NSLayoutPriorityDefaultHigh;
    hug.active = YES;
    shortest = [content.heightAnchor constraintEqualToConstant:0];
    shortest.priority = 500;
    shortest.active = YES;
    return p;
}

- (NSStackView *)folderGroup
{
    NSButton *choose = [NSButton buttonWithTitle:@"Choose Folder…" target:self action:@selector(choose:)];
    NSStackView *statusRow, *stack;
    self.path = text_label(@"", [NSFont systemFontOfSize:12], C_TEXT);
    self.path.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.path.selectable = YES;
    self.path.accessibilityLabel = @"Game folder";
    [self.path setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    self.statusIcon = [NSImageView new];
    [self.statusIcon.widthAnchor constraintEqualToConstant:16].active = YES;
    [self.statusIcon.heightAnchor constraintEqualToConstant:16].active = YES;
    self.status = text_label(@"", [NSFont systemFontOfSize:12], C_HINT);
    self.status.lineBreakMode = NSLineBreakByWordWrapping;
    self.status.preferredMaxLayoutWidth = RIGHT_WIDTH - 2 * PANEL_PAD - 24;
    statusRow = [NSStackView stackViewWithViews:@[ self.statusIcon, self.status ]];
    statusRow.alignment = NSLayoutAttributeTop;
    statusRow.spacing = 8;
    stack = [NSStackView stackViewWithViews:@[ group_title(@"Game folder"), self.path, statusRow, choose ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10;
    [stack setCustomSpacing:14 afterView:statusRow];
    [self.path.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    [statusRow.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}

- (void)showFolder
{
    NSString *problem = nil;
    NSImage *img;
    unsigned color;
    if (dir[0] == 0)
    {
        self.path.stringValue = @"No folder selected";
        self.path.textColor = rgb(C_HINT);
        self.path.toolTip = nil;
        problem = @"Choose the game folder of the GOG release (the folder that holds the Engine folder).";
        dirOK = false;
    }
    else
    {
        char why[1024];
        self.path.stringValue = [nsstr(dir) stringByAbbreviatingWithTildeInPath];
        self.path.textColor = rgb(C_TEXT);
        self.path.toolTip = nsstr(dir);
        dirOK = check(dir, why, sizeof why) != 0;
        if (!dirOK) problem = nsstr(why);
    }
    color = dirOK ? C_OK : C_WARN;
    img = [NSImage imageWithSystemSymbolName:dirOK ? @"checkmark.circle.fill" : @"exclamationmark.triangle.fill"
                    accessibilityDescription:dirOK ? @"Ready" : @"Problem"];
    if (img == nil) launcher_fail(@"A symbol of macOS is missing.", @"The status symbol cannot be loaded.");
    self.statusIcon.image = img;
    self.statusIcon.contentTintColor = rgb(color);
    self.status.stringValue = dirOK ? @"The GOG release of the game. Ready to play." : problem;
    self.status.textColor = dirOK ? rgb(C_TEXT) : rgb(C_WARN);
    self.play.enabled = dirOK;
    [self updateIcon];
}

// The icon of the GOG release from the selected folder. The window shows the title without the icon until a usable
// folder is selected.
- (void)updateIcon
{
    NSString *file;
    NSImage *img;
    if (!dirOK)
    {
        self.icon.image = nil;
        self.icon.hidden = YES;
        iconDir[0] = 0;
        return;
    }
    if (strcmp(iconDir, dir) == 0) return;
    file = [nsstr(dir) stringByAppendingPathComponent:@GAME_ICON];
    img = [[NSImage alloc] initWithContentsOfFile:file];
    if (img == nil)
    {
        self.icon.image = nil;
        self.icon.hidden = YES;
        iconDir[0] = 0;
        [self showProblem:[NSString stringWithFormat:@"The game icon cannot be shown: %@ cannot be read.", file]];
        return;
    }
    self.icon.image = img;
    self.icon.hidden = NO;
    memcpy(iconDir, dir, sizeof iconDir);
}

- (void)showValues
{
    for (NSNumber *off in self.popups)
        [self.popups[off] selectItemAtIndex:*setting_field(&values, off.unsignedLongValue)];
}

- (void)build
{
    NSTextField *title, *subtitle, *versionNote;
    NSView *rule, *content, *ground;
    NSStackView *titles, *header, *columns, *leftStack, *rightStack, *buttons, *notes, *actions, *root;
    NSButton *restore;
    CAGradientLayer *sea;
    self.popups = [NSMutableDictionary new];

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 840, 680)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable |
                                                        NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Call of Cthulhu";
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.backgroundColor = rgb(C_GROUND);
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;

    title = spaced_label(@"CALL OF CTHULHU", font_named(@"Baskerville-SemiBold", 38), C_TEXT, 3.0);
    subtitle = spaced_label(@"DARK CORNERS OF THE EARTH", font_named(@"Baskerville", 14), C_SUBTITLE, 3.2);
    titles = [NSStackView stackViewWithViews:@[ title, subtitle ]];
    titles.orientation = NSUserInterfaceLayoutOrientationVertical;
    titles.alignment = NSLayoutAttributeLeading;
    titles.spacing = 0;
    self.icon = [NSImageView new];
    self.icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.icon.accessibilityLabel = @"The icon of Call of Cthulhu: Dark Corners of the Earth";
    self.icon.hidden = YES;
    [self.icon.widthAnchor constraintEqualToConstant:64].active = YES;
    [self.icon.heightAnchor constraintEqualToConstant:64].active = YES;
    header = [NSStackView stackViewWithViews:@[ self.icon, titles ]];
    header.alignment = NSLayoutAttributeCenterY;
    header.spacing = 16;
    // The header keeps its height when the icon shows.
    [header.heightAnchor constraintGreaterThanOrEqualToConstant:64].active = YES;

    rule = [NSView new];
    rule.wantsLayer = YES;
    rule.layer.backgroundColor = [rgb(C_BRASS) colorWithAlphaComponent:0.55].CGColor;
    [rule.heightAnchor constraintEqualToConstant:1].active = YES;

    self.play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
    self.play.keyEquivalent = @"\r";
    self.play.controlSize = NSControlSizeLarge;
    self.play.bezelColor = rgb(C_VERDIGRIS);
    self.play.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    [self.play.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;

    restore = [NSButton buttonWithTitle:@"Restore Defaults" target:self action:@selector(restore:)];
    restore.controlSize = NSControlSizeLarge;
    versionNote = text_label(@"The game uses these settings when it starts. You can also change them in the game, in "
                             @"Options > Enhancements.",
                             [NSFont systemFontOfSize:11], C_HINT);
    self.banner = text_label(@"", [NSFont systemFontOfSize:12 weight:NSFontWeightMedium], C_WARN);
    self.banner.lineBreakMode = NSLineBreakByWordWrapping;
    self.banner.preferredMaxLayoutWidth = RIGHT_WIDTH;
    self.banner.hidden = YES;
    versionNote.lineBreakMode = NSLineBreakByWordWrapping;
    versionNote.preferredMaxLayoutWidth = RIGHT_WIDTH;
    notes = [NSStackView stackViewWithViews:@[ self.banner, versionNote ]];
    notes.orientation = NSUserInterfaceLayoutOrientationVertical;
    notes.alignment = NSLayoutAttributeLeading;
    notes.spacing = 4;
    buttons = [NSStackView stackViewWithViews:@[ restore, self.play ]];
    buttons.distribution = NSStackViewDistributionEqualSpacing;
    actions = [NSStackView stackViewWithViews:@[ notes, buttons ]];
    actions.orientation = NSUserInterfaceLayoutOrientationVertical;
    actions.alignment = NSLayoutAttributeLeading;
    actions.spacing = 12;
    [buttons.widthAnchor constraintEqualToAnchor:actions.widthAnchor].active = YES;
    [notes.widthAnchor constraintEqualToAnchor:actions.widthAnchor].active = YES;
    [actions.widthAnchor constraintEqualToConstant:RIGHT_WIDTH].active = YES;
    {
        NSLayoutConstraint *shortest = [actions.heightAnchor constraintEqualToConstant:0];
        shortest.priority = 500; // as in panel()
        shortest.active = YES;
    }

    // Left: Graphics at the top, Sound and Mouse at the bottom. Right: Game folder and Game at the top, the notes and
    // the buttons at the bottom. The two columns have the same height, so Play is level with the bottom of the left
    // column, and the free space of a column is between its top and bottom groups.
    leftStack = [NSStackView new];
    leftStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    leftStack.alignment = NSLayoutAttributeLeading;
    leftStack.spacing = 16;
    [leftStack addView:panel([self groupWithTitle:@"Graphics" rows:graphics_rows count:NGRAPHICS panelWidth:LEFT_WIDTH], LEFT_WIDTH)
             inGravity:NSStackViewGravityTop];
    [leftStack addView:panel([self groupWithTitle:@"Sound and mouse" rows:sound_rows count:NSOUND panelWidth:LEFT_WIDTH], LEFT_WIDTH)
             inGravity:NSStackViewGravityBottom];
    [leftStack setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    rightStack = [NSStackView new];
    rightStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    rightStack.alignment = NSLayoutAttributeLeading;
    rightStack.spacing = 16;
    [rightStack addView:panel([self folderGroup], RIGHT_WIDTH) inGravity:NSStackViewGravityTop];
    [rightStack addView:panel([self groupWithTitle:@"Game" rows:game_rows count:NGAMEROWS panelWidth:RIGHT_WIDTH], RIGHT_WIDTH)
              inGravity:NSStackViewGravityTop];
    [rightStack addView:actions inGravity:NSStackViewGravityBottom];
    [rightStack setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    columns = [NSStackView stackViewWithViews:@[ leftStack, rightStack ]];
    columns.alignment = NSLayoutAttributeTop;
    columns.spacing = 16;
    [leftStack.heightAnchor constraintEqualToAnchor:rightStack.heightAnchor].active = YES;

    root = [NSStackView stackViewWithViews:@[ header, rule, columns ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 16;
    [root setCustomSpacing:14 afterView:header];
    [root setCustomSpacing:22 afterView:rule];
    root.translatesAutoresizingMaskIntoConstraints = NO;

    // The ground: the dark of deep sea water, lighter at the top.
    content = self.window.contentView;
    ground = [NSView new];
    ground.translatesAutoresizingMaskIntoConstraints = NO;
    sea = [CAGradientLayer layer];
    sea.colors = @[ (id)rgb(C_GROUND).CGColor, (id)rgb(C_GROUND_TOP).CGColor ]; // bottom to top (layer y up)
    ground.layer = sea;    // a layer-hosting view: the layer first, then wantsLayer
    ground.wantsLayer = YES;
    [content addSubview:ground];
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:28],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-28],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:40],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-24],
        [rule.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [header.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [ground.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [ground.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [ground.topAnchor constraintEqualToAnchor:content.topAnchor],
        [ground.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    [self.window setContentSize:content.fittingSize];
    self.window.initialFirstResponder = self.play;
}

- (void)showProblem:(NSString *)text
{
    self.banner.stringValue = text != nil ? text : @"";
    self.banner.hidden = text == nil;
}

- (IBAction)choose:(id)sender
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    (void)sender;
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.allowsMultipleSelection = NO;
    p.prompt = @"Choose";
    p.message = @"Choose the game folder of the GOG release of Call of Cthulhu: Dark Corners of the Earth (the folder "
                @"that holds the Engine folder).";
    if (dir[0] != 0) p.directoryURL = [NSURL fileURLWithPath:nsstr(dir) isDirectory:YES];
    [p beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse r) {
                const char *fs;
                if (r != NSModalResponseOK || p.URL == nil) return;
                fs = p.URL.fileSystemRepresentation;
                if (fs == NULL || strlen(fs) >= sizeof self->dir)
                {
                    [self showProblem:@"The folder path is too long. Choose a folder with a shorter path."];
                    return;
                }
                memcpy(self->dir, fs, strlen(fs) + 1);
                [self showProblem:nil];
                [self showFolder];
              }];
}

- (IBAction)restore:(id)sender
{
    (void)sender;
    for (NSNumber *off in self.popups)
        [self.popups[off] selectItemAtIndex:coc_enh_keys[key_index(off.unsignedLongValue)].def];
    [self showProblem:nil];
}

// End the event loop of coc_launcher_run. The loop waits for the next event, so a posted event wakes it when the
// action came from outside the handling of an event (a menu action that AppKit sends later from the run loop).
- (void)finishWithPlay:(BOOL)play
{
    NSEvent *wake;
    if (self.finished) return;
    self.finished = YES;
    self.playChosen = play;
    wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                              location:NSZeroPoint
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                               subtype:0
                                 data1:0
                                 data2:0];
    if (wake == nil) launcher_fail(@"The window cannot be closed.", @"The wake event cannot be made.");
    [NSApp postEvent:wake atStart:NO];
}

- (IBAction)play:(id)sender
{
    RtEnh chosen = values;
    char problem[1400];
    (void)sender;
    [self showFolder];
    if (!dirOK) return;
    for (NSNumber *off in self.popups)
    {
        NSInteger sel = self.popups[off].indexOfSelectedItem;
        const CocEnhKey *k = &coc_enh_keys[key_index(off.unsignedLongValue)];
        if (sel < 0 || sel > k->max) launcher_fail(@"The launcher is damaged.", @"A menu has no valid value selected.");
        *setting_field(&chosen, off.unsignedLongValue) = (int32_t)sel;
    }
    if (!coc_enh_write(&chosen, problem, sizeof problem))
    {
        [self showProblem:nsstr(problem)];
        return;
    }
    values = chosen;
    [self finishWithPlay:YES];
}

- (IBAction)quit:(id)sender
{
    (void)sender;
    [self finishWithPlay:NO];
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    [self finishWithPlay:NO];
    return YES;
}
@end

// The application menu while the window is open: Quit closes the window. The runtime makes its own menu when the
// game starts (rt_cocoa_run).
static void launcher_menu(CoCLauncher *l)
{
    NSMenu *bar = [NSMenu new], *app = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new], *quit;
    quit = [[NSMenuItem alloc] initWithTitle:@"Quit Call of Cthulhu" action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = l;
    [app addItem:quit];
    appItem.submenu = app;
    [bar addItem:appItem];
    NSApp.mainMenu = bar;
}

// Copy the standardized form of path to out. Returns false (out unchanged) when it does not fit.
static bool copy_standard(char *out, size_t n, NSString *path)
{
    const char *fs = path.stringByStandardizingPath.fileSystemRepresentation;
    if (fs == NULL || strlen(fs) >= n) return false;
    memcpy(out, fs, strlen(fs) + 1);
    return true;
}

int coc_launcher_run(const char *initial, coc_launcher_check_fn check, char *out, size_t n)
{
    CoCLauncher *l;
    NSString *remembered;
    char problem[1400];
    int ok;
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // finishLaunching makes the window accessible (VoiceOver: AppKit answers accessibility requests only after
        // it) and installs the quit Apple event handler of AppKit: Quit in the Dock, log out and shut down end the
        // process. The Prototype launcher called it only when the game started, so its window was not accessible
        // (the accessibility requests failed with kAXErrorCannotComplete). rt_cocoa_run calls it again when the game
        // starts.
        [NSApp finishLaunching];
        l = [CoCLauncher new];
        l->check = check;
        l->dir[0] = 0;
        l->iconDir[0] = 0;
        // The folder that the window shows first: the folder of the last Play, else initial. When the folder of the last
        // Play cannot be used (its files were moved or removed) and initial can, the window shows initial; else it
        // shows the folder of the last Play with its problem.
        // Both paths are standardized (no "." or ".." parts), so that the window shows and remembers the plain path.
        {
            char first[LAUNCHER_PATH_LEN], why[1024];
            bool have_first = false, have_initial = false;
            remembered = [NSUserDefaults.standardUserDefaults stringForKey:DEFAULTS_GAME_DIR];
            if (initial != NULL)
                have_initial = copy_standard(first, sizeof first, nsstr(initial));
            if (initial != NULL && !have_initial) launcher_fail(@"The game folder path is too long.", nsstr(initial));
            if (remembered != nil) have_first = copy_standard(l->dir, sizeof l->dir, remembered);
            if (have_first && have_initial && !check(l->dir, why, sizeof why) && check(first, why, sizeof why))
                memcpy(l->dir, first, strlen(first) + 1);
            else if (!have_first && have_initial)
                memcpy(l->dir, first, strlen(first) + 1);
        }
        [l build];
        launcher_menu(l);
        ok = coc_enh_read(&l->values, problem, sizeof problem);
        [l showValues];
        if (!ok)
            [l showProblem:[nsstr(problem) stringByAppendingString:@" The window shows the valid values. Play writes a new file."]];
        [l showFolder];
        [l.window center];
        [NSApp activate];
        [l.window makeKeyAndOrderFront:nil];
        // An event loop in the default run loop mode, not runModalForWindow:. AppKit sends the action of a main-menu
        // item (Quit, Command-Q) later from the run loop, and a modal loop (NSModalPanelRunLoopMode) does not run it.
        while (!l.finished)
        {
            @autoreleasepool
            {
                NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:NSDate.distantFuture
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
                if (e != nil && e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
            }
        }
        [l.window orderOut:nil];
        if (!l.playChosen) return 0;
        if (strlen(l->dir) >= n) launcher_fail(@"The game folder path is too long.", nsstr(l->dir));
        memcpy(out, l->dir, strlen(l->dir) + 1);
        [NSUserDefaults.standardUserDefaults setObject:nsstr(l->dir) forKey:DEFAULTS_GAME_DIR];
    }
    return 1;
}
