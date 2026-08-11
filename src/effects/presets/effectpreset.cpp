#include "effects/presets/effectpreset.h"

#include <QHash>
#include <functional>

#include "effects/backends/effectsbackend.h"
#include "effects/effectslot.h"
#include "effects/presets/effectxmlelements.h"
#include "util/xml.h"

EffectPreset::EffectPreset()
        : m_backendType(EffectBackendType::Unknown),
          m_dMetaParameter(0.0),
          m_dWet(1.0) {
}

EffectPreset::EffectPreset(const QDomElement& effectElement)
        : EffectPreset() {
    // effectElement can come from untrusted input from the filesystem, so do not DEBUG_ASSERT
    if (effectElement.tagName() != EffectXml::kEffect) {
        return;
    }
    if (!effectElement.hasChildNodes()) {
        return;
    }

    m_id = XmlParse::selectNodeQString(effectElement, EffectXml::kEffectId);
    m_backendType = EffectsBackend::backendTypeFromString(
            XmlParse::selectNodeQString(effectElement,
                    EffectXml::kEffectBackendType));
    m_dMetaParameter = XmlParse::selectNodeDouble(effectElement, EffectXml::kEffectMetaParameter);
    // Absent in presets written before per-slot wet existed, and
    // selectNodeDouble gives 0 for a missing node -- which would silently mute
    // every effect in an old rack. Treat missing as fully wet.
    const QDomElement wetElement = XmlParse::selectElement(effectElement, EffectXml::kEffectWet);
    m_dWet = wetElement.isNull()
            ? 1.0
            : XmlParse::selectNodeDouble(effectElement, EffectXml::kEffectWet);

    QDomElement parametersElement =
            XmlParse::selectElement(effectElement, EffectXml::kParametersRoot);
    QDomNodeList parametersList = parametersElement.childNodes();

    for (int i = 0; i < parametersList.count(); ++i) {
        QDomNode parameterNode = parametersList.at(i);
        if (parameterNode.isElement()) {
            QDomElement parameterElement = parameterNode.toElement();
            EffectParameterPreset parameterPreset(parameterElement);
            m_effectParameterPresets.append(parameterElement);
        }
    }
}

EffectPreset::EffectPreset(const EffectSlotPointer pEffectSlot)
        : m_id(pEffectSlot->id()),
          m_backendType(pEffectSlot->backendType()),
          m_dMetaParameter(pEffectSlot->getMetaParameter()),
          m_dWet(pEffectSlot->getWet()) {
    // Parameters are reloaded in the order they are saved, so the order of
    // loaded effects must be preserved.
    int numTypes = static_cast<int>(EffectParameterType::NumTypes);
    for (int parameterTypeId = 0; parameterTypeId < numTypes; ++parameterTypeId) {
        const EffectParameterType parameterType =
                static_cast<EffectParameterType>(parameterTypeId);
        const auto& loadedParameters = pEffectSlot->getLoadedParameters().value(parameterType);
        for (const auto& pParameter : loadedParameters) {
            m_effectParameterPresets.append(EffectParameterPreset(pParameter, false));
        }
        const auto& hiddenParameters = pEffectSlot->getHiddenParameters().value(parameterType);
        for (const auto& pParameter : hiddenParameters) {
            m_effectParameterPresets.append(EffectParameterPreset(pParameter, true));
        }
    }
}

EffectPreset::EffectPreset(const EffectManifestPointer pManifest)
        : m_id(pManifest->id()),
          m_backendType(pManifest->backendType()),
          m_dMetaParameter(pManifest->metaknobDefault()),
          m_dWet(1.0) {
    for (const auto& pParameterManifest : pManifest->parameters()) {
        m_effectParameterPresets.append(EffectParameterPreset(pParameterManifest));
    }
}

const QDomElement EffectPreset::toXml(QDomDocument* doc) const {
    QDomElement effectElement = doc->createElement(EffectXml::kEffect);
    if (m_id.isEmpty()) {
        return effectElement;
    }

    XmlParse::addElement(
            *doc,
            effectElement,
            EffectXml::kEffectMetaParameter,
            QString::number(m_dMetaParameter));
    XmlParse::addElement(
            *doc,
            effectElement,
            EffectXml::kEffectWet,
            QString::number(m_dWet));
    XmlParse::addElement(
            *doc,
            effectElement,
            EffectXml::kEffectId,
            m_id);
    XmlParse::addElement(
            *doc,
            effectElement,
            EffectXml::kEffectBackendType,
            EffectsBackend::backendTypeToString(m_backendType));

    QDomElement parametersElement = doc->createElement(EffectXml::kParametersRoot);
    for (const auto& pParameter : std::as_const(m_effectParameterPresets)) {
        parametersElement.appendChild(pParameter.toXml(doc));
    }
    effectElement.appendChild(parametersElement);

    return effectElement;
}

void EffectPreset::updateParametersFrom(const EffectPreset& other) {
    DEBUG_ASSERT(backendType() == other.backendType());

    // we use a std::reference_wrapper as optimization so we don't copy and
    // store the entire object when we need to copy later anyways
    // TODO(XXX): Replace QHash with <std::flat_map>
    QHash<QString, std::reference_wrapper<const EffectParameterPreset>> parameterPresetLookup;
    // we build temporary hashtable to reduce the algorithmic complexity of the
    // lookup later. A plain std::find_if (O(n)) would've resulted in O(n^n)
    // parameter updating. The hashtable has O(1) lookup with O(n) updating,
    // though with more constant overhead. Since 3rd-party EffectPresets could
    // have hundreds of parameters, we can't afford O(n^2) lookups.
    // TODO(XXX): measure overhead and possibly implement fallback to lower
    // overhead O(n^2) solution for small presets.
    for (const EffectParameterPreset& preset : other.m_effectParameterPresets) {
        parameterPresetLookup.insert(preset.id(), std::ref(preset));
    }

    for (EffectParameterPreset& parameterToUpdate : m_effectParameterPresets) {
        const auto parameterToCopyIt = parameterPresetLookup.constFind(parameterToUpdate.id());
        if (parameterToCopyIt != parameterPresetLookup.constEnd()) {
            parameterToUpdate = *parameterToCopyIt;
        }
    }
}
