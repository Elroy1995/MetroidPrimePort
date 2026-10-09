
// iOS front end for the port: a title menu with "Select ISO" and "Play", plus
// Apple's document picker for choosing the disc image. Used by platform/main.cpp.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdio>
#include <cstring>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

typedef void (^MPPickBlock)(NSString* path);

static UIColor* MPOrange() { return [UIColor colorWithRed:1.0 green:0.54 blue:0.12 alpha:1.0]; }
static UIColor* MPCyan() { return [UIColor colorWithRed:0.25 green:0.85 blue:1.0 alpha:1.0]; }
static UIColor* MPGreen() { return [UIColor colorWithRed:0.36 green:1.0 blue:0.62 alpha:1.0]; }
static UIColor* MPAmber() { return [UIColor colorWithRed:1.0 green:0.71 blue:0.33 alpha:1.0]; }
static UIColor* MPRed() { return [UIColor colorWithRed:1.0 green:0.42 blue:0.42 alpha:1.0]; }

static UIViewController* TopViewController() {
  UIWindow* window = nil;
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if ([scene isKindOfClass:[UIWindowScene class]]) {
      for (UIWindow* candidate in ((UIWindowScene*)scene).windows) {
        if (candidate.isKeyWindow) {
          window = candidate;
          break;
        }
      }
    }
    if (window != nil) {
      break;
    }
  }
  if (window == nil) {
    window = UIApplication.sharedApplication.windows.firstObject;
  }
  UIViewController* controller = window.rootViewController;
  while (controller.presentedViewController != nil) {
    controller = controller.presentedViewController;
  }
  return controller;
}

// ---------------------------------------------------------------- disc picker

@interface MPDiscPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) MPPickBlock done;
@end

// The controller only holds its delegate weakly, so keep it alive here.
static MPDiscPicker* sPicker = nil;

@implementation MPDiscPicker
- (void)finish:(NSString*)path {
  MPPickBlock done = self.done;
  sPicker = nil;
  if (done != nil) {
    done(path);
  }
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* picked = urls.firstObject;
  if (picked == nil) {
    [self finish:nil];
    return;
  }
  // The system already copied the file into tmp; moving it into Documents is instant.
  NSFileManager* fm = [NSFileManager defaultManager];
  NSURL* documents = [fm URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
  NSURL* destination = [documents URLByAppendingPathComponent:picked.lastPathComponent];
  NSString* result = picked.path;
  if (destination != nil && ![picked.path isEqualToString:destination.path]) {
    [fm removeItemAtURL:destination error:nil];
    NSError* error = nil;
    if ([fm moveItemAtURL:picked toURL:destination error:&error]) {
      result = destination.path;
    } else {
      std::fprintf(stderr, "metroid_prime_port: could not move the picked disc into Documents: %s\n",
                   error.localizedDescription.UTF8String);
    }
  }
  [self finish:result];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  [self finish:nil];
}
@end

static void PresentPicker(UIViewController* from, MPPickBlock done) {
  MPDiscPicker* picker = [[MPDiscPicker alloc] init];
  picker.done = done;
  sPicker = picker;
  UIDocumentPickerViewController* controller =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeItem ] asCopy:YES];
  controller.delegate = picker;
  controller.allowsMultipleSelection = NO;
  [from presentViewController:controller animated:YES completion:nil];
}

// Kept for callers that only want the picker.
extern "C" void MPIosPickDisc(void (*done)(const char* path, void* user), void* user) {
  void (^present)(void) = ^{
    UIViewController* top = TopViewController();
    if (top == nil) {
      if (done != nullptr) {
        done(nullptr, user);
      }
      return;
    }
    PresentPicker(top, ^(NSString* path) {
      if (done != nullptr) {
        done(path != nil ? path.UTF8String : nullptr, user);
      }
    });
  };
  if ([NSThread isMainThread]) {
    present();
  } else {
    dispatch_async(dispatch_get_main_queue(), present);
  }
}

// ------------------------------------------------------------- disc inspection

typedef NS_ENUM(NSInteger, MPDiscState) { MPDiscMissing, MPDiscWrong, MPDiscOK, MPDiscUnchecked };

// Mirrors the port's own check (GM8E01, disc 0, version 0) for .iso and .gcm files.
// Other formats (rvz, wbfs, ...) can't be read here, so the game decides.
static MPDiscState InspectDisc(NSString* path, NSString** detail) {
  NSDictionary* attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
  if (attributes == nil) {
    *detail = path.lastPathComponent;
    return MPDiscMissing;
  }
  const double gigabytes = (double)attributes.fileSize / 1073741824.0;
  *detail = [NSString stringWithFormat:@"%@  ·  %.2f GB", path.lastPathComponent, gigabytes];
  NSString* extension = path.pathExtension.lowercaseString;
  if (![extension isEqualToString:@"iso"] && ![extension isEqualToString:@"gcm"]) {
    return MPDiscUnchecked;
  }
  NSFileHandle* handle = [NSFileHandle fileHandleForReadingAtPath:path];
  if (handle == nil) {
    return MPDiscMissing;
  }
  NSData* head = [handle readDataOfLength:8];
  [handle closeFile];
  const unsigned char* bytes = (const unsigned char*)head.bytes;
  const BOOL ok = head.length == 8 && std::memcmp(bytes, "GM8E01", 6) == 0 && bytes[6] == 0 && bytes[7] == 0;
  return ok ? MPDiscOK : MPDiscWrong;
}

// ------------------------------------------------------------------- launcher

static UIButton* MakeButton(NSString* title, BOOL filled) {
  UIButton* button = [UIButton buttonWithType:UIButtonTypeCustom];
  [button setTitle:title forState:UIControlStateNormal];
  button.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightBold];
  button.layer.cornerRadius = 12;
  button.layer.borderWidth = 1.5;
  button.layer.borderColor = MPOrange().CGColor;
  if (filled) {
    button.backgroundColor = MPOrange();
    [button setTitleColor:[UIColor colorWithRed:0.08 green:0.04 blue:0.0 alpha:1.0] forState:UIControlStateNormal];
    [button setTitleColor:[UIColor colorWithRed:0.08 green:0.04 blue:0.0 alpha:1.0] forState:UIControlStateDisabled];
  } else {
    button.backgroundColor = UIColor.clearColor;
    [button setTitleColor:MPOrange() forState:UIControlStateNormal];
    [button setTitleColor:[MPOrange() colorWithAlphaComponent:0.4] forState:UIControlStateDisabled];
  }
  [button.heightAnchor constraintEqualToConstant:46].active = YES;
  return button;
}

@interface MPLauncherController : UIViewController
@property(nonatomic, copy) NSString* discPath;
@property(nonatomic, assign) BOOL playRequested;
@end

@implementation MPLauncherController {
  CAGradientLayer* _gradient;
  UIView* _dot;
  UILabel* _statusTitle;
  UILabel* _statusDetail;
  UIButton* _selectButton;
  UIButton* _playButton;
  BOOL _playable;
}

- (BOOL)prefersStatusBarHidden {
  return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
  return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskLandscape;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.blackColor;

  _gradient = [CAGradientLayer layer];
  _gradient.colors = @[
    (id)[UIColor colorWithRed:0.02 green:0.03 blue:0.06 alpha:1.0].CGColor,
    (id)[UIColor colorWithRed:0.04 green:0.11 blue:0.15 alpha:1.0].CGColor
  ];
  [self.view.layer addSublayer:_gradient];

  UILabel* title = [[UILabel alloc] init];
  title.attributedText = [[NSAttributedString alloc]
      initWithString:@"METROID PRIME"
          attributes:@{
            NSFontAttributeName : [UIFont systemFontOfSize:36 weight:UIFontWeightHeavy],
            NSForegroundColorAttributeName : MPOrange(),
            NSKernAttributeName : @6.0
          }];
  title.textAlignment = NSTextAlignmentCenter;
  title.adjustsFontSizeToFitWidth = YES;
  title.minimumScaleFactor = 0.6;

  UILabel* subtitle = [[UILabel alloc] init];
  subtitle.attributedText = [[NSAttributedString alloc]
      initWithString:@"NATIVE  iOS  PORT"
          attributes:@{
            NSFontAttributeName : [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold],
            NSForegroundColorAttributeName : [MPCyan() colorWithAlphaComponent:0.85],
            NSKernAttributeName : @4.0
          }];
  subtitle.textAlignment = NSTextAlignmentCenter;

  UIView* ruleHolder = [[UIView alloc] init];
  UIView* rule = [[UIView alloc] init];
  rule.translatesAutoresizingMaskIntoConstraints = NO;
  rule.backgroundColor = MPOrange();
  rule.layer.shadowColor = MPOrange().CGColor;
  rule.layer.shadowOpacity = 0.9;
  rule.layer.shadowRadius = 8;
  rule.layer.shadowOffset = CGSizeZero;
  [ruleHolder addSubview:rule];
  [NSLayoutConstraint activateConstraints:@[
    [ruleHolder.heightAnchor constraintEqualToConstant:2],
    [rule.centerXAnchor constraintEqualToAnchor:ruleHolder.centerXAnchor],
    [rule.widthAnchor constraintEqualToConstant:120],
    [rule.topAnchor constraintEqualToAnchor:ruleHolder.topAnchor],
    [rule.bottomAnchor constraintEqualToAnchor:ruleHolder.bottomAnchor]
  ]];

  _dot = [[UIView alloc] init];
  _dot.layer.cornerRadius = 5;
  [_dot.widthAnchor constraintEqualToConstant:10].active = YES;
  [_dot.heightAnchor constraintEqualToConstant:10].active = YES;
  _statusTitle = [[UILabel alloc] init];
  _statusTitle.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
  _statusTitle.textColor = UIColor.whiteColor;
  _statusDetail = [[UILabel alloc] init];
  _statusDetail.font = [UIFont systemFontOfSize:12];
  _statusDetail.textColor = [UIColor colorWithWhite:1.0 alpha:0.6];
  _statusDetail.lineBreakMode = NSLineBreakByTruncatingMiddle;
  UIStackView* texts = [[UIStackView alloc] initWithArrangedSubviews:@[ _statusTitle, _statusDetail ]];
  texts.axis = UILayoutConstraintAxisVertical;
  texts.spacing = 2;
  UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:@[ _dot, texts ]];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.spacing = 12;
  row.alignment = UIStackViewAlignmentCenter;
  row.translatesAutoresizingMaskIntoConstraints = NO;
  UIView* card = [[UIView alloc] init];
  card.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.06];
  card.layer.cornerRadius = 14;
  card.layer.borderWidth = 1;
  card.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.14].CGColor;
  [card addSubview:row];
  [NSLayoutConstraint activateConstraints:@[
    [row.topAnchor constraintEqualToAnchor:card.topAnchor constant:12],
    [row.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12],
    [row.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
    [row.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16]
  ]];

  _selectButton = MakeButton(@"SELECT ISO", NO);
  _playButton = MakeButton(@"PLAY", YES);
  _playButton.layer.shadowColor = MPOrange().CGColor;
  _playButton.layer.shadowRadius = 12;
  _playButton.layer.shadowOffset = CGSizeZero;
  [_selectButton addTarget:self action:@selector(selectTapped) forControlEvents:UIControlEventTouchUpInside];
  [_playButton addTarget:self action:@selector(playTapped) forControlEvents:UIControlEventTouchUpInside];

  UIStackView* stack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ title, subtitle, ruleHolder, card, _selectButton, _playButton ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 12;
  [stack setCustomSpacing:18 afterView:ruleHolder];
  stack.translatesAutoresizingMaskIntoConstraints = NO;

  UILabel* footer = [[UILabel alloc] init];
  footer.text = @"Bring your own Metroid Prime (USA, v1.00) disc image. Fan-made port, not affiliated with Nintendo.";
  footer.font = [UIFont systemFontOfSize:10];
  footer.textColor = [UIColor colorWithWhite:1.0 alpha:0.35];
  footer.textAlignment = NSTextAlignmentCenter;
  footer.numberOfLines = 2;
  footer.translatesAutoresizingMaskIntoConstraints = NO;

  [self.view addSubview:stack];
  [self.view addSubview:footer];
  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  NSLayoutConstraint* width = [stack.widthAnchor constraintEqualToConstant:420];
  width.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [stack.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor constant:-8],
    [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24],
    [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-24],
    width,
    [footer.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
    [footer.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
    [footer.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-6]
  ]];

  [self refresh];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  _gradient.frame = self.view.bounds;
}

- (void)refresh {
  if (self.discPath.length == 0) {
    _playable = NO;
    _dot.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.35];
    _statusTitle.text = @"No disc image selected";
    _statusDetail.text = @"Tap SELECT ISO and choose your Metroid Prime disc image.";
  } else {
    NSString* detail = nil;
    switch (InspectDisc(self.discPath, &detail)) {
    case MPDiscOK:
      _playable = YES;
      _dot.backgroundColor = MPGreen();
      _statusTitle.text = @"Metroid Prime (USA) v1.00  ·  ready";
      break;
    case MPDiscUnchecked:
      _playable = YES;
      _dot.backgroundColor = MPCyan();
      _statusTitle.text = @"Disc image selected";
      break;
    case MPDiscWrong:
      _playable = NO;
      _dot.backgroundColor = MPAmber();
      _statusTitle.text = @"This isn't Metroid Prime (USA) v1.00";
      break;
    case MPDiscMissing:
      _playable = NO;
      _dot.backgroundColor = MPRed();
      _statusTitle.text = @"Disc image not found";
      break;
    }
    _statusDetail.text = detail;
  }
  _playButton.enabled = _playable;
  _playButton.alpha = _playable ? 1.0 : 0.4;
  [_playButton.layer removeAnimationForKey:@"pulse"];
  if (_playable) {
    _playButton.layer.shadowOpacity = 0.6;
    CABasicAnimation* pulse = [CABasicAnimation animationWithKeyPath:@"shadowOpacity"];
    pulse.fromValue = @0.15;
    pulse.toValue = @0.9;
    pulse.duration = 1.2;
    pulse.autoreverses = YES;
    pulse.repeatCount = HUGE_VALF;
    [_playButton.layer addAnimation:pulse forKey:@"pulse"];
  } else {
    _playButton.layer.shadowOpacity = 0;
  }
}

- (void)selectTapped {
  [[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight] impactOccurred];
  __weak MPLauncherController* weakSelf = self;
  PresentPicker(self, ^(NSString* path) {
    MPLauncherController* strongSelf = weakSelf;
    if (strongSelf != nil && path != nil) {
      strongSelf.discPath = path;
      [strongSelf refresh];
    }
  });
}

- (void)playTapped {
  if (!_playable) {
    return;
  }
  [[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium] impactOccurred];
  _selectButton.enabled = NO;
  _playButton.enabled = NO;
  [_playButton setTitle:@"STARTING…" forState:UIControlStateDisabled];
  self.playRequested = YES;
}
@end

static MPLauncherController* sLauncher = nil;
static int sPresentAttempts = 0;

static void TryPresentLauncher() {
  if (sLauncher == nil || sLauncher.presentingViewController != nil) {
    return;
  }
  UIViewController* top = TopViewController();
  if (top == nil || top.view.window == nil) {
    return;
  }
  [top presentViewController:sLauncher animated:NO completion:nil];
}

extern "C" void MPIosLauncherShow(const char* currentDisc) {
  MPLauncherController* controller = [[MPLauncherController alloc] init];
  controller.discPath = currentDisc != nullptr ? [NSString stringWithUTF8String:currentDisc] : nil;
  controller.modalPresentationStyle = UIModalPresentationFullScreen;
  sLauncher = controller;
  sPresentAttempts = 0;
  TryPresentLauncher();
}

// 0: still waiting, 1: Play was pressed (path filled in), -1: the menu could not be shown.
extern "C" int MPIosLauncherPoll(char* out, size_t size) {
  if (sLauncher == nil) {
    return -1;
  }
  if (sLauncher.presentingViewController == nil) {
    TryPresentLauncher();
    if (++sPresentAttempts > 900) {
      sLauncher = nil;
      return -1;
    }
    return 0;
  }
  if (sLauncher.playRequested && sLauncher.discPath.length > 0) {
    std::snprintf(out, size, "%s", sLauncher.discPath.UTF8String);
    return 1;
  }
  return 0;
}

extern "C" void MPIosLauncherHide(void) {
  MPLauncherController* controller = sLauncher;
  sLauncher = nil;
  if (controller != nil && controller.presentingViewController != nil) {
    [controller dismissViewControllerAnimated:NO completion:nil];
  }
}
