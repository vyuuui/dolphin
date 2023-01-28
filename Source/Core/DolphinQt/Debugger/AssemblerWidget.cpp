#include "DolphinQt/Debugger/AssemblerWidget.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>

#include <filesystem>
#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"

#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

#include "DolphinQt/Debugger/AssemblyEditor.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
using namespace Common::GekkoAssembler;

QString HtmlFormatErrorLoc(AssemblerError const& err)
{
  return QStringLiteral(
             "<span style=\"color: red; font-weight: bold\">Error</span> on line %1 col %2")
      .arg(err.line + 1)
      .arg(err.col + 1);
}

QString HtmlFormatErrorLine(AssemblerError const& err)
{
  QString line_pre_error =
      QString::fromStdString(std::string(err.error_line.substr(0, err.col))).toHtmlEscaped();
  QString line_error =
      QString::fromStdString(std::string(err.error_line.substr(err.col, err.len))).toHtmlEscaped();
  QString line_post_error =
      QString::fromStdString(std::string(err.error_line.substr(err.col + err.len))).toHtmlEscaped();

  return QStringLiteral("<span style=\"font-family:'monospace';font-size:16px\">"
                        "<pre>%1<u><span style=\"color:red;font-weight:bold\">%2</span></u>%3</pre>"
                        "</span>")
      .arg(line_pre_error)
      .arg(line_error)
      .arg(line_post_error);
}

QString HtmlFormatMessage(AssemblerError const& err)
{
  return QStringLiteral("<span>%1</span>").arg(QString::fromStdString(err.message).toHtmlEscaped());
}

void DeserializeBlock(CodeBlock const& blk, std::ostringstream& out_str, bool pad4)
{
  size_t i = 0;
  for (; i < blk.instructions.size(); i++)
  {
    out_str << fmt::format("{:02x}", blk.instructions[i]);
    if (i % 8 == 7)
    {
      out_str << '\n';
    }
    else if (i % 4 == 3)
    {
      out_str << ' ';
    }
  }
  if (pad4)
  {
    bool did_pad = false;
    for (; i % 4 != 0; i++)
    {
      out_str << "00";
      did_pad = true;
    }

    if (did_pad)
    {
      out_str << (i % 8 == 0 ? '\n' : ' ');
    }
  }
  else if (i % 8 != 7)
  {
    out_str << '\n';
  }
}

void DeserializeToRaw(std::vector<CodeBlock> const& blocks, std::ostringstream& out_str)
{
  for (auto const& blk : blocks)
  {
    if (blk.instructions.empty())
    {
      continue;
    }

    out_str << fmt::format("# Block {:08x}\n", blk.block_address);
    DeserializeBlock(blk, out_str, false);
  }
}

void DeserializeToAr(std::vector<CodeBlock> const& blocks, std::ostringstream& out_str)
{
  for (auto const& blk : blocks)
  {
    if (blk.instructions.empty())
    {
      continue;
    }

    size_t i = 0;
    for (; i < blk.instructions.size() - 3; i += 4)
    {
      // type=NormalCode, subtype=SUB_RAM_WRITE, size=32bit
      const u32 ar_addr = ((blk.block_address + i) & 0x1ffffff) | 0x04000000;
      out_str << fmt::format("{:08x} {:02x}{:02x}{:02x}{:02x}\n", ar_addr, blk.instructions[i],
                             blk.instructions[i + 1], blk.instructions[i + 2],
                             blk.instructions[i + 3]);
    }

    for (; i < blk.instructions.size(); i++)
    {
      // type=NormalCode, subtype=SUB_RAM_WRITE, size=8bit
      const u32 ar_addr = ((blk.block_address + i) & 0x1ffffff);
      out_str << fmt::format("{:08x} 000000{:02x}\n", ar_addr, blk.instructions[i]);
    }
  }
}

void DeserializeToGecko(std::vector<CodeBlock> const& blocks, std::ostringstream& out_str)
{
  DeserializeToAr(blocks, out_str);
}

void DeserializeToGeckoExec(std::vector<CodeBlock> const& blocks, std::ostringstream& out_str)
{
  for (auto const& blk : blocks)
  {
    if (blk.instructions.empty())
    {
      continue;
    }

    u32 nlines = 1 + static_cast<u32>((blk.instructions.size() - 1) / 8);
    bool ret_on_newline = false;
    if (blk.instructions.size() % 8 == 0 || blk.instructions.size() % 8 > 4)
    {
      // Append extra line for blr
      nlines++;
      ret_on_newline = true;
    }

    out_str << fmt::format("c0000000 {:08x}\n", nlines);
    DeserializeBlock(blk, out_str, true);
    if (ret_on_newline)
    {
      out_str << "4e800020 00000000\n";
    }
    else
    {
      out_str << "4e800020\n";
    }
  }
}

void DeserializeToGeckoTramp(std::vector<CodeBlock> const& blocks, std::ostringstream& out_str)
{
  for (auto const& blk : blocks)
  {
    if (blk.instructions.empty())
    {
      continue;
    }

    u32 inject_addr = (blk.block_address & 0x1ffffff) | 0x02000000;
    u32 nlines = 1 + static_cast<u32>((blk.instructions.size() - 1) / 8);
    bool padding_on_newline = false;
    if (blk.instructions.size() % 8 == 0 || blk.instructions.size() % 8 > 4)
    {
      // Append extra line for nop+branchback
      nlines++;
      padding_on_newline = true;
    }

    out_str << fmt::format("c{:07x} {:08x}\n", inject_addr, nlines);
    DeserializeBlock(blk, out_str, true);
    if (padding_on_newline)
    {
      out_str << "60000000 00000000\n";
    }
    else
    {
      out_str << "00000000\n";
    }
  }
}
}  // namespace

AssemblerWidget::AssemblerWidget(QWidget* parent) : QDockWidget(parent), m_unnamed_editor_count(0)
{
  setWindowTitle(tr("Assembler"));
  setObjectName(QStringLiteral("assemblerwidget"));

  setHidden(!Settings::Instance().IsAssemblerVisible() ||
            !Settings::Instance().IsDebugModeEnabled());

  this->setVisible(true);
  CreateWidgets();

  restoreGeometry(
      Settings::GetQSettings().value(QStringLiteral("assemblerwidget/geometry")).toByteArray());
  setFloating(Settings::GetQSettings().value(QStringLiteral("assemblerwidget/floating")).toBool());

  connect(&Settings::Instance(), &Settings::AssemblerVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });

  connect(&Settings::Instance(), &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsAssemblerVisible());
  });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &AssemblerWidget::OnEmulationStateChanged);
  connect(m_asm_tabs, &QTabWidget::tabCloseRequested, this, &AssemblerWidget::OnTabClose);

  auto* save_shortcut = new QShortcut(QKeySequence::Save, this);
  // Save should only activate if the active tab is in focus
  save_shortcut->connect(save_shortcut, &QShortcut::activated, this, [this] {
    if (m_asm_tabs->currentIndex() != -1 && m_asm_tabs->currentWidget()->hasFocus())
    {
      OnSave();
    }
  });

  ConnectWidgets();
}

void AssemblerWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetAssemblerVisible(false);
}

AssemblerWidget::~AssemblerWidget()
{
  auto& settings = Settings::GetQSettings();

  settings.setValue(QStringLiteral("assemblerwidget/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("assemblerwidget/floating"), isFloating());
}

void AssemblerWidget::CreateWidgets()
{
  m_asm_tabs = new QTabWidget;
  m_toolbar = new QToolBar;
  m_output_type = new QComboBox;
  m_output_box = new QPlainTextEdit;
  m_error_box = new QTextEdit;
  m_address_line = new QLineEdit;
  m_copy_output_button = new QPushButton;

  m_asm_tabs->setTabsClosable(true);

  // Initialize toolbar and actions
  m_toolbar->setIconSize(QSize(32, 32));
  m_toolbar->setContentsMargins(0, 0, 0, 0);
  m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  m_open = m_toolbar->addAction(tr("Open"), this, &AssemblerWidget::OnOpen);
  m_new = m_toolbar->addAction(tr("New"), this, &AssemblerWidget::OnNew);
  m_assemble = m_toolbar->addAction(tr("Assemble"), this, [this] {
    std::vector<CodeBlock> unused;
    OnAssemble(unused);
  });
  m_inject = m_toolbar->addAction(tr("Inject"), this, &AssemblerWidget::OnInject);
  m_save = m_toolbar->addAction(tr("Save"), this, &AssemblerWidget::OnSave);

  m_inject->setEnabled(false);
  m_save->setEnabled(false);
  m_assemble->setEnabled(false);

  m_new->setIcon(Resources::GetScaledThemeIcon("new"));
  m_open->setIcon(Resources::GetScaledThemeIcon("openasm"));
  m_save->setIcon(Resources::GetScaledThemeIcon("save"));
  m_assemble->setIcon(Resources::GetScaledThemeIcon("assemble"));
  m_inject->setIcon(Resources::GetScaledThemeIcon("inject"));

  // Initialize input, output, error text areas
  auto palette = m_output_box->palette();
  palette.setColor(QPalette::Base, QColor::fromRgb(180, 180, 180));
  m_output_box->setPalette(palette);
  m_error_box->setPalette(palette);

  QFont mono_font(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  QFont error_font(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
  mono_font.setPointSize(12);
  error_font.setPointSize(12);
  QFontMetrics mono_metrics(mono_font);
  QFontMetrics err_metrics(mono_font);

  m_output_box->setFont(mono_font);
  m_error_box->setFont(error_font);
  m_output_box->setReadOnly(true);
  m_error_box->setReadOnly(true);

  m_error_box->setFixedHeight(err_metrics.height() * 3 + mono_metrics.height());
  m_output_box->setFixedWidth(mono_metrics.horizontalAdvance(QLatin1Char('0')) * OUTPUT_BOX_WIDTH);
  m_error_box->setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);

  // Initialize output format selection box
  m_output_type->addItem(QStringLiteral("Raw"));
  m_output_type->addItem(QStringLiteral("AR Code"));
  m_output_type->addItem(QStringLiteral("Gecko (Write)"));
  m_output_type->addItem(QStringLiteral("Gecko (Execute)"));
  m_output_type->addItem(QStringLiteral("Gecko (Trampoline)"));
  m_copy_output_button->setIcon(Resources::GetScaledThemeIcon("clipboard"));

  // Setup layouts
  auto* addr_input_layout = new QHBoxLayout;
  addr_input_layout->addWidget(new QLabel(QStringLiteral("Base Address")));
  addr_input_layout->addWidget(m_address_line);

  auto* output_extra_layout = new QHBoxLayout;
  output_extra_layout->addWidget(m_output_type);
  output_extra_layout->addWidget(m_copy_output_button);

  QWidget* address_input_box = new QWidget();
  address_input_box->setLayout(addr_input_layout);
  addr_input_layout->setContentsMargins(0, 0, 0, 0);

  QWidget* output_extra_box = new QWidget();
  output_extra_box->setLayout(output_extra_layout);
  output_extra_layout->setContentsMargins(0, 0, 0, 0);

  auto* assembler_layout = new QGridLayout;
  assembler_layout->addWidget(m_toolbar, 0, 0, 1, 2);
  {
    auto* input_group = new QGroupBox(tr("Input"));
    auto* layout = new QVBoxLayout;
    input_group->setLayout(layout);
    layout->addWidget(m_asm_tabs);
    layout->addWidget(address_input_box);
    assembler_layout->addWidget(input_group, 1, 0, 1, 1);
  }
  {
    auto* output_group = new QGroupBox(tr("Output"));
    auto* layout = new QGridLayout;
    output_group->setLayout(layout);
    layout->addWidget(m_output_box, 0, 0);
    layout->addWidget(output_extra_box, 1, 0);
    assembler_layout->addWidget(output_group, 1, 1, 1, 1);
    output_group->setSizePolicy(
        QSizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Expanding));
  }
  {
    auto* error_group = new QGroupBox(tr("Error Log"));
    auto* layout = new QHBoxLayout;
    error_group->setLayout(layout);
    layout->addWidget(m_error_box);
    assembler_layout->addWidget(error_group, 2, 0, 1, 2);
    error_group->setSizePolicy(
        QSizePolicy(QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Fixed));
  }

  QWidget* widget = new QWidget;
  widget->setLayout(assembler_layout);
  setWidget(widget);
}

void AssemblerWidget::ConnectWidgets()
{
  m_output_box->connect(m_output_box, &QPlainTextEdit::updateRequest, this, [this] {
    if (m_output_box->verticalScrollBar()->isVisible())
    {
      m_output_box->setFixedWidth(m_output_box->fontMetrics().horizontalAdvance(QLatin1Char('0')) *
                                      OUTPUT_BOX_WIDTH +
                                  m_output_box->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
    }
    else
    {
      m_output_box->setFixedWidth(m_output_box->fontMetrics().horizontalAdvance(QLatin1Char('0')) *
                                  OUTPUT_BOX_WIDTH);
    }
  });
  m_copy_output_button->connect(m_copy_output_button, &QPushButton::released, this,
                                &AssemblerWidget::OnCopyOutput);
  m_address_line->connect(m_address_line, &QLineEdit::textChanged, this,
                          &AssemblerWidget::OnBaseAddressChanged);
  m_asm_tabs->connect(m_asm_tabs, &QTabWidget::currentChanged, this, &AssemblerWidget::OnTabChange);
}

void AssemblerWidget::OnAssemble(std::vector<CodeBlock>& asm_out)
{
  if (m_asm_tabs->currentIndex() == -1)
  {
    return;
  }
  AsmEditor* active_editor = GetEditor(m_asm_tabs->currentIndex());

  AsmKind kind = AsmKind::Raw;
  m_error_box->clear();
  m_output_box->clear();
  switch (m_output_type->currentIndex())
  {
  case 0:
    kind = AsmKind::Raw;
    break;
  case 1:
    kind = AsmKind::ActionReplay;
    break;
  case 2:
    kind = AsmKind::Gecko;
    break;
  case 3:
    kind = AsmKind::GeckoExec;
    break;
  case 4:
    kind = AsmKind::GeckoTrampoline;
    break;
  }

  bool good;
  u32 base_address = m_address_line->text().toUInt(&good, 16);
  if (!good)
  {
    base_address = 0;
    m_error_box->append(QStringLiteral(
        "<span style=\"color:#ffcc00\">Warning</span> invalid base address, defaulting to 0"));
  }

  std::string contents = active_editor->toPlainText().toStdString();
  auto result = Assemble(contents, base_address);
  if (is_failure(result))
  {
    m_error_box->clear();
    asm_out.clear();

    m_error_box->append(HtmlFormatErrorLoc(get_failure(result)));
    m_error_box->append(HtmlFormatErrorLine(get_failure(result)));
    m_error_box->append(HtmlFormatMessage(get_failure(result)));
    asm_out.clear();
    return;
  }

  auto& blocks = get_t(result);
  std::ostringstream str_contents;
  switch (kind)
  {
  case AsmKind::Raw:
    DeserializeToRaw(blocks, str_contents);
    break;
  case AsmKind::ActionReplay:
    DeserializeToAr(blocks, str_contents);
    break;
  case AsmKind::Gecko:
    DeserializeToGecko(blocks, str_contents);
    break;
  case AsmKind::GeckoExec:
    DeserializeToGeckoExec(blocks, str_contents);
    break;
  case AsmKind::GeckoTrampoline:
    DeserializeToGeckoTramp(blocks, str_contents);
    break;
  }

  m_output_box->appendPlainText(QString::fromStdString(str_contents.str()));
  m_output_box->moveCursor(QTextCursor::MoveOperation::Start);
  m_output_box->ensureCursorVisible();

  asm_out = std::move(get_t(result));
}

void AssemblerWidget::OnCopyOutput()
{
  QApplication::clipboard()->setText(m_output_box->toPlainText());
}

void AssemblerWidget::OnOpen()
{
  std::string default_dir = File::GetUserPath(D_ASM_ROOT_IDX);
  QStringList paths = DolphinFileDialog::getOpenFileNames(
      this, tr("Select a File"), QString::fromStdString(default_dir),
      QStringLiteral("%1 (*.s *.S *.asm);;%2 (*)")
          .arg(tr("All Assembly files"))
          .arg(tr("All Files")));
  if (paths.isEmpty())
  {
    return;
  }

  std::optional<int> show_index;
  for (auto path : paths)
  {
    show_index = std::nullopt;
    for (int i = 0; i < m_asm_tabs->count(); i++)
    {
      AsmEditor* editor = GetEditor(i);
      if (editor->PathsMatch(path))
      {
        show_index = i;
        break;
      }
    }

    if (!show_index)
    {
      NewEditor(path);
    }
  }

  if (show_index)
  {
    m_asm_tabs->setCurrentIndex(*show_index);
  }
}

void AssemblerWidget::OnNew()
{
  NewEditor();
}

void AssemblerWidget::OnInject()
{
  std::vector<CodeBlock> asm_result;
  OnAssemble(asm_result);
  for (auto const& blk : asm_result)
  {
    if (!PowerPC::HostIsRAMAddress(blk.block_address) || blk.instructions.empty())
    {
      continue;
    }

    PowerPC::debug_interface.SetPatch(blk.block_address, blk.instructions);
  }
}

void AssemblerWidget::OnSave()
{
  if (m_asm_tabs->currentIndex() == -1)
  {
    return;
  }
  AsmEditor* active_editor = GetEditor(m_asm_tabs->currentIndex());

  QString save_path = active_editor->Path();
  if (save_path.isEmpty())
  {
    const std::string default_dir = File::GetUserPath(D_ASM_ROOT_IDX);
    const QString asm_filter = QStringLiteral("%1 (*.S)").arg(tr("Assembly File"));
    const QString all_filter = QStringLiteral("%2 (*)").arg(tr("All Files"));

    QString selected_filter;
    save_path = DolphinFileDialog::getSaveFileName(
        this, tr("Save File to"), QString::fromStdString(default_dir),
        QStringLiteral("%1;;%2").arg(asm_filter).arg(all_filter), &selected_filter);

    if (save_path.isEmpty())
    {
      return;
    }

    if (selected_filter == asm_filter &&
        std::filesystem::path(save_path.toStdString()).extension().empty())
    {
      save_path.append(QStringLiteral(".S"));
    }
  }

  active_editor->SaveFile(save_path);
}

void AssemblerWidget::OnBaseAddressChanged()
{
  if (m_asm_tabs->currentIndex() == -1)
  {
    return;
  }
  AsmEditor* active_editor = GetEditor(m_asm_tabs->currentIndex());

  active_editor->SetBaseAddress(m_address_line->text());
}

void AssemblerWidget::OnTabChange(int index)
{
  if (index == -1)
  {
    m_address_line->clear();
    return;
  }
  AsmEditor* active_editor = GetEditor(index);

  m_address_line->setText(active_editor->BaseAddress());
}

QString AssemblerWidget::TabTextForEditor(AsmEditor* editor, bool with_dirty)
{
  ASSERT(editor != nullptr);
  QString dirtyFlag = QStringLiteral();
  if (editor->IsDirty() && with_dirty)
  {
    dirtyFlag = QStringLiteral(" *");
  }

  if (editor->Path().isEmpty())
  {
    if (editor->EditorNum() == 0)
    {
      return QStringLiteral("New File%1").arg(dirtyFlag);
    }
    return QStringLiteral("New File (%1)%2").arg(editor->EditorNum() + 1).arg(dirtyFlag);
  }
  return QStringLiteral("%1%2").arg(editor->FileName()).arg(dirtyFlag);
}

AsmEditor* AssemblerWidget::GetEditor(int idx)
{
  return qobject_cast<AsmEditor*>(m_asm_tabs->widget(idx));
}

void AssemblerWidget::NewEditor(QString const& path)
{
  AsmEditor* new_editor =
      new AsmEditor(path, path.isEmpty() ? AllocateTabNum() : INVALID_EDITOR_NUM);
  if (!path.isEmpty() && !new_editor->LoadFromPath())
  {
    ModalMessageBox::warning(this, tr("Failed to open file"),
                             tr("Failed to read the contents of file\n\n"
                                "\"%1\"")
                                 .arg(path));
    delete new_editor;
    return;
  }

  const int tab_idx = m_asm_tabs->addTab(new_editor, QStringLiteral());
  new_editor->connect(new_editor, &AsmEditor::PathChanged, this,
                      [this] { UpdateTabText(qobject_cast<AsmEditor*>(sender())); });
  new_editor->connect(new_editor, &AsmEditor::DirtyChanged, this,
                      [this] { UpdateTabText(qobject_cast<AsmEditor*>(sender())); });

  m_asm_tabs->setTabText(tab_idx, TabTextForEditor(new_editor, true));

  if (m_save && m_assemble)
  {
    m_save->setEnabled(true);
    m_assemble->setEnabled(true);
  }

  m_asm_tabs->setCurrentIndex(tab_idx);
}

void AssemblerWidget::OnEmulationStateChanged(Core::State state)
{
  m_inject->setEnabled(state != Core::State::Uninitialized);
}

void AssemblerWidget::OnTabClose(int index)
{
  ASSERT(index < m_asm_tabs->count());
  AsmEditor* editor = GetEditor(index);

  if (editor->IsDirty())
  {
    const int result = ModalMessageBox::question(this, tr("Unsaved Changes"),
                                                 tr("There are unsaved changes in \"%1\".\n\n"
                                                    "Are you sure you want to close?")
                                                     .arg(TabTextForEditor(editor, false)));
    if (result != QMessageBox::Yes)
    {
      return;
    }
  }

  CloseTab(index, editor);
}

void AssemblerWidget::CloseTab(int index, AsmEditor* editor)
{
  FreeTabNum(editor->EditorNum());

  m_asm_tabs->removeTab(index);
  editor->deleteLater();

  if (m_asm_tabs->count() == 0 && m_save && m_assemble)
  {
    m_save->setEnabled(false);
    m_assemble->setEnabled(false);
  }
}

int AssemblerWidget::AllocateTabNum()
{
  auto min_it = std::min_element(m_free_editor_nums.begin(), m_free_editor_nums.end());
  if (min_it == m_free_editor_nums.end())
  {
    return m_unnamed_editor_count++;
  }

  const int min = *min_it;
  m_free_editor_nums.erase(min_it);
  return min;
}

void AssemblerWidget::FreeTabNum(int num)
{
  if (num != INVALID_EDITOR_NUM)
  {
    m_free_editor_nums.push_back(num);
  }
}

void AssemblerWidget::UpdateTabText(AsmEditor* editor)
{
  int tab_idx = 0;
  for (; tab_idx < m_asm_tabs->count(); tab_idx++)
  {
    if (m_asm_tabs->widget(tab_idx) == editor)
    {
      break;
    }
  }
  ASSERT(tab_idx < m_asm_tabs->count());

  m_asm_tabs->setTabText(tab_idx, TabTextForEditor(editor, true));
}
