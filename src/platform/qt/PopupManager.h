/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QExplicitlySharedDataPointer>
#include <QPointer>
#include <QSharedData>

#include <functional>
#include <memory>
#include <type_traits>

#include "CorePointer.h"

namespace QGBA {

class CoreController;

class PopupManagerBase {
public:
	PopupManagerBase(const PopupManagerBase&) = default;
	virtual ~PopupManagerBase() = default;

	QWidget* construct();
	void show();
	inline void operator()() { show(); }

protected:
	class Private : public QSharedData, public CoreConsumer {
	public:
		Private(PopupManagerBase* pub) : pub(pub) {}
		Private(const Private& other) = default;

		using CoreConsumer::m_controller;

		PopupManagerBase* pub;
		QMetaObject::Connection stopConnection;
		bool keepAlive = false;

		virtual QWidget* window() const = 0;

		void onCoreDetached(std::shared_ptr<CoreController>) override;
		void onCoreAttached(std::shared_ptr<CoreController>) override;
	};

	PopupManagerBase(Private* d);

	virtual void constructImpl() = 0;

	virtual Private* d() const { return m_d.data(); }
	virtual Private* d() { return m_d.data(); }

private:
	QExplicitlySharedDataPointer<Private> m_d;
};

template <class WINDOW>
class PopupManager : public PopupManagerBase {
	static_assert(std::is_convertible<WINDOW*, QWidget*>::value, "class must derive from QWidget");

	template <typename T>
	struct is_window_function {
		static constexpr bool value = std::is_convertible<T, std::function<WINDOW*()>>::value;
	};

protected:
	class Private;
	Private* d() const override { return static_cast<Private*>(PopupManagerBase::d()); }
	Private* d() override { return static_cast<Private*>(PopupManagerBase::d()); }

public:
	PopupManager() : PopupManagerBase(new Private(this)) {}
	PopupManager(CorePointerSource& source) : PopupManagerBase(new Private(this)) {
		d()->m_controller.setSource(&source);
	}
	PopupManager(const PopupManager&) = default;

	bool isNull() const { return d()->ptr.isNull(); }
	WINDOW* operator->() const { return d()->ptr; }
	WINDOW& operator*() const { return &d()->ptr; }
	operator WINDOW*() const { return d()->ptr; }

	PopupManager& setKeepAlive(bool keepAlive) { d()->keepAlive = keepAlive; return *this; }

	// Construct by invoking a lambda that returns WINDOW*
	template <typename FUNC, typename std::enable_if<is_window_function<FUNC>::value, bool>::type = true>
	PopupManager& constructWith(const FUNC& ctor) {
		d()->construct = ctor;
		return *this;
	}

	// Construct by calling the WINDOW constructor with the specified args
	template <typename... Args>
	PopupManager& constructWith(Args... args) {
		return constructWith([=]() -> WINDOW* { return new WINDOW(args...); });
	}

protected:
	virtual void constructImpl() override {
		Private* d = this->d();
		if (!d->construct) {
			qWarning("No valid constructor specified for popup");
			return;
		}
		WINDOW* w = d->construct();
		if (!w) {
			qWarning("Constructor did not return a window");
			return;
		}
		d->ptr = w;
	}

	class Private : public PopupManagerBase::Private {
		template <class T = WINDOW>
		struct Ctors {
			static constexpr bool useController = std::is_constructible<T, CorePointerSource*>::value;
			static constexpr bool useDefault = !useController && std::is_default_constructible<T>::value;
			static constexpr bool noDefault = !useController && !useDefault;
		};

	public:
		template<class T = WINDOW>
		Private(PopupManagerBase* pub, typename std::enable_if<Ctors<T>::useDefault>::type* = 0)
		: PopupManagerBase::Private(pub), construct([]{ return new WINDOW(); }) {}

		template<class T = WINDOW>
		Private(PopupManagerBase* pub, typename std::enable_if<Ctors<T>::useController>::type* = 0)
		: PopupManagerBase::Private(pub), construct([this]{ return new WINDOW(m_controller.source()); }) {}

		template<class T = WINDOW>
		Private(PopupManagerBase* pub, typename std::enable_if<Ctors<T>::noDefault>::type* = 0)
		: PopupManagerBase::Private(pub) {}

		~Private() {
			if (ptr && !keepAlive) {
				ptr->close();
				ptr->deleteLater();
			}
		}

		virtual QWidget* window() const override { return ptr.data(); }

		QPointer<WINDOW> ptr;
		std::function<WINDOW*()> construct;
	};
};

}
