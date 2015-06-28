#include "common/common_pch.h"

#include <QAbstractItemView>
#include <QStringList>

#include "common/qt.h"
#include "mkvtoolnix-gui/app.h"
#include "mkvtoolnix-gui/util/country_combo_box.h"
#include "mkvtoolnix-gui/util/util.h"

namespace mtx { namespace gui { namespace Util {

CountryComboBox::CountryComboBox(QWidget *parent)
  : ComboBoxBase{parent}
{
}

CountryComboBox::~CountryComboBox() {
}

ComboBoxBase &
CountryComboBox::setup(bool withEmpty,
                        QString const &emptyTitle) {
  m_withEmpty  = withEmpty;
  m_emptyTitle = emptyTitle;

  if (m_withEmpty)
    addItem(m_emptyTitle, Q(""));

  auto &commonCountries = App::commonIso3166_1Alpha2Countries();
  if (!commonCountries.empty()) {
    for (auto const &country : commonCountries)
      addItem(country.first, country.second);

    insertSeparator(commonCountries.size() + (withEmpty ? 1 : 0));
  }

  for (auto const &country : App::iso3166_1Alpha2Countries())
    addItem(country.first, country.second);

  view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  return *this;
}

}}}
