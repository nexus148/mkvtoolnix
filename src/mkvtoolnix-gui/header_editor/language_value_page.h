#ifndef MTX_MKVTOOLNIX_GUI_HEADER_EDITOR_LANGUAGE_VALUE_PAGE_H
#define MTX_MKVTOOLNIX_GUI_HEADER_EDITOR_LANGUAGE_VALUE_PAGE_H

#include "common/common_pch.h"

#include "mkvtoolnix-gui/header_editor/value_page.h"

namespace mtx { namespace gui {

namespace Util {
class LanguageComboBox;
}

namespace HeaderEditor {

class Tab;

class LanguageValuePage: public ValuePage {
public:
  Util::LanguageComboBox *m_cbValue{};
  std::string m_originalValue;
  int m_originalValueIdx{};

public:
  LanguageValuePage(Tab &parent, PageBase &topLevelPage, EbmlMaster &master, EbmlCallbacks const &callbacks, translatable_string_c const &title, translatable_string_c const &description);
  virtual ~LanguageValuePage();

  virtual QWidget *createInputControl() override;
  virtual QString originalValueAsString() const override;
  virtual QString currentValueAsString() const override;
  virtual void resetValue() override;
  virtual bool validateValue() const override;
  virtual void copyValueToElement() override;
};

}}}

#endif  // MTX_MKVTOOLNIX_GUI_HEADER_EDITOR_LANGUAGE_VALUE_PAGE_H
