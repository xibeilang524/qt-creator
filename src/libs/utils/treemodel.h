/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include "utils_global.h"

#include <QAbstractItemModel>

#include <functional>

namespace Utils {

class BaseTreeModel;

class QTCREATOR_UTILS_EXPORT TreeItem
{
public:
    TreeItem();
    virtual ~TreeItem();

    virtual QVariant data(int column, int role) const;
    virtual bool setData(int column, const QVariant &data, int role);
    virtual Qt::ItemFlags flags(int column) const;

    virtual bool hasChildren() const;
    virtual bool canFetchMore() const;
    virtual void fetchMore() {}

    TreeItem *parent() const { return m_parent; }

    void prependChild(TreeItem *item);
    void appendChild(TreeItem *item);
    void insertChild(int pos, TreeItem *item);
    void removeChildren();
    void sortChildren(const std::function<bool(const TreeItem *, const TreeItem *)> &cmp);
    void update();
    void updateAll();
    void updateColumn(int column);
    void expand();
    TreeItem *firstChild() const;
    TreeItem *lastChild() const;
    int level() const;

    int childCount() const { return m_children.size(); }
    TreeItem *childAt(int index) const;
    QVector<TreeItem *> children() const { return m_children; }
    QModelIndex index() const;
    QAbstractItemModel *model() const;

    template <class T, class Predicate>
    void forSelectedChildren(const Predicate &pred) const {
        foreach (TreeItem *item, m_children) {
            if (pred(static_cast<T>(item)))
                item->forSelectedChildren<T, Predicate>(pred);
        }
    }

    template <class T, typename Predicate>
    void forAllChildren(const Predicate &pred) const {
        foreach (TreeItem *item, m_children) {
            pred(static_cast<T>(item));
            item->forAllChildren<T, Predicate>(pred);
        }
    }

    // Levels are 1-based: Child at Level 1 is an immediate child.

    template <class T, typename Predicate>
    void forChildrenAtLevel(int level, Predicate pred) const
    {
        if (level == 1) {
            foreach (TreeItem *item, m_children)
                pred(static_cast<T>(item));
        } else {
            foreach (TreeItem *item, m_children)
                item->forChildrenAtLevel<T, Predicate>(level - 1, pred);
        }
    }

    template <class T, typename Predicate>
    T findChildAtLevel(int level, Predicate pred) const
    {
        if (level == 1) {
            foreach (TreeItem *item, m_children)
                if (pred(static_cast<T>(item)))
                    return static_cast<T>(item);
        } else {
            foreach (TreeItem *item, m_children) {
                if (auto found = item->findChildAtLevel<T, Predicate>(level - 1, pred))
                    return found;
            }
        }
        return 0;
    }

    template <class T, typename Predicate>
    T findAnyChild(Predicate pred) const {
        foreach (TreeItem *item, m_children) {
            if (pred(static_cast<T>(item)))
                return static_cast<T>(item);
            if (T found = item->findAnyChild<T>(pred))
                return found;
        }
        return 0;
    }

private:
    TreeItem(const TreeItem &) = delete;
    void operator=(const TreeItem &) = delete;

    void clear();
    void propagateModel(BaseTreeModel *m);

    TreeItem *m_parent; // Not owned.
    BaseTreeModel *m_model; // Not owned.
    QVector<TreeItem *> m_children; // Owned.
    friend class BaseTreeModel;
};

// A TreeItem with children all of the same type.
template <class ChildType, class ParentType = TreeItem>
class TypedTreeItem : public TreeItem
{
public:
    ChildType *childAt(int index) const { return static_cast<ChildType *>(TreeItem::childAt(index)); }

    void sortChildren(const std::function<bool(const ChildType *, const ChildType *)> &lessThan)
    {
        return TreeItem::sortChildren([lessThan](const TreeItem *a, const TreeItem *b) {
            return lessThan(static_cast<const ChildType *>(a), static_cast<const ChildType *>(b));
        });
    }

    template <typename Predicate>
    void forAllChildren(const Predicate &pred) const {
        return TreeItem::forAllChildren<ChildType *, Predicate>(pred);
    }

    template <typename Predicate>
    void forFirstLevelChildren(Predicate pred) const {
        return TreeItem::forChildrenAtLevel<ChildType *, Predicate>(1, pred);
    }

    template <typename Predicate>
    ChildType *findFirstLevelChild(Predicate pred) const {
        return TreeItem::findChildAtLevel<ChildType *, Predicate>(1, pred);
    }

    ParentType *parent() const {
        return static_cast<ParentType *>(TreeItem::parent());
    }
};

class QTCREATOR_UTILS_EXPORT StaticTreeItem : public TreeItem
{
public:
    StaticTreeItem(const QStringList &displays);
    StaticTreeItem(const QString &display);

    QVariant data(int column, int role) const override;
    Qt::ItemFlags flags(int column) const override;

private:
    QStringList m_displays;
};

// A general purpose multi-level model where each item can have its
// own (TreeItem-derived) type.
class QTCREATOR_UTILS_EXPORT BaseTreeModel : public QAbstractItemModel
{
    Q_OBJECT

protected:
    explicit BaseTreeModel(QObject *parent = 0);
    explicit BaseTreeModel(TreeItem *root, QObject *parent = 0);
    ~BaseTreeModel() override;

    void setHeader(const QStringList &displays);
    void setHeaderToolTip(const QStringList &tips);
    void clear();

    TreeItem *rootItem() const;
    void setRootItem(TreeItem *item);
    TreeItem *itemForIndex(const QModelIndex &) const;
    QModelIndex indexForItem(const TreeItem *needle) const;

    int topLevelItemCount() const;
    int rowCount(const QModelIndex &idx = QModelIndex()) const override;
    int columnCount(const QModelIndex &idx) const override;

    bool setData(const QModelIndex &idx, const QVariant &data, int role) override;
    QVariant data(const QModelIndex &idx, int role) const override;
    QModelIndex index(int, int, const QModelIndex &idx = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &idx) const override;
    Qt::ItemFlags flags(const QModelIndex &idx) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    bool hasChildren(const QModelIndex &idx) const override;

    bool canFetchMore(const QModelIndex &idx) const override;
    void fetchMore(const QModelIndex &idx) override;

    TreeItem *takeItem(TreeItem *item); // item is not destroyed.
    void destroyItem(TreeItem *item); // item is destroyed.

signals:
    void requestExpansion(QModelIndex);

protected:
    friend class TreeItem;

    TreeItem *m_root; // Owned.
    QStringList m_header;
    QStringList m_headerToolTip;
    int m_columnCount;
};

namespace Internal {

// SelectType<N, T0, T1, T2, ...> selects the Nth type from the list
// If there are not enough types in the list, 'TreeItem' is used.
template<int N, typename ...All> struct SelectType;

template<int N, typename First, typename ...Rest> struct SelectType<N, First, Rest...>
{
    using Type = typename SelectType<N - 1, Rest...>::Type;
};

template<typename First, typename ...Rest> struct SelectType<0, First, Rest...>
{
    using Type = First;
};

template<int N> struct SelectType<N>
{
    using Type = TreeItem;
};


// BestItem<T0, T1, T2, ... > selects T0 if all types are equal and 'TreeItem' otherwise
template<typename ...All> struct BestItemType;

template<typename First, typename Second, typename ...Rest> struct BestItemType<First, Second, Rest...>
{
    using Type = TreeItem;
};

template<typename First, typename ...Rest> struct BestItemType<First, First, Rest...>
{
    using Type = typename BestItemType<First, Rest...>::Type;
};

template<typename First> struct BestItemType<First>
{
    using Type = First;
};

template<> struct BestItemType<>
{
    using Type = TreeItem;

};

} // namespace Internal

// A multi-level model with possibly uniform types per level.
template <typename ...LevelItemTypes>
class TreeModel : public BaseTreeModel
{
public:
    using RootItem = typename Internal::SelectType<0, LevelItemTypes...>::Type;
    using BestItem = typename Internal::BestItemType<LevelItemTypes...>::Type;

    explicit TreeModel(QObject *parent = 0) : BaseTreeModel(new RootItem, parent) {}
    explicit TreeModel(RootItem *root, QObject *parent = 0) : BaseTreeModel(root, parent) {}

    using BaseTreeModel::clear;
    using BaseTreeModel::columnCount;
    using BaseTreeModel::data;
    using BaseTreeModel::destroyItem;
    using BaseTreeModel::hasChildren;
    using BaseTreeModel::index;
    using BaseTreeModel::indexForItem;
    using BaseTreeModel::rowCount;
    using BaseTreeModel::setData;
    using BaseTreeModel::setHeader;
    using BaseTreeModel::setHeaderToolTip;
    using BaseTreeModel::takeItem;
    using BaseTreeModel::topLevelItemCount;

    template <int Level, class Predicate>
    void forItemsAtLevel(const Predicate &pred) const {
        m_root->forChildrenAtLevel<typename Internal::SelectType<Level, LevelItemTypes...>::Type *>(Level, pred);
    }

    template <int Level, class Predicate>
    typename Internal::SelectType<Level, LevelItemTypes...>::Type *findItemAtLevel(const Predicate &pred) const {
        return m_root->findChildAtLevel<typename Internal::SelectType<Level, LevelItemTypes...>::Type *>(Level, pred);
    }

    RootItem *rootItem() const {
        return static_cast<RootItem *>(BaseTreeModel::rootItem());
    }

    template<int Level>
    typename Internal::SelectType<Level, LevelItemTypes...>::Type *itemForIndexAtLevel(const QModelIndex &idx) const {
       TreeItem *item = BaseTreeModel::itemForIndex(idx);
        return item && item->level() == Level ? static_cast<typename Internal::SelectType<Level, LevelItemTypes...>::Type *>(item) : 0;
    }

    BestItem *nonRootItemForIndex(const QModelIndex &idx) const {
        TreeItem *item = BaseTreeModel::itemForIndex(idx);
        return item && item->parent() ? static_cast<BestItem *>(item) : 0;
    }

    template <class Predicate>
    BestItem *findNonRooItem(const Predicate &pred) const {
        TreeItem *root = this->rootItem();
        return root->findAnyChild<BestItem *>(pred);
    }

    template <class Predicate>
    void forSelectedItems(const Predicate &pred) const {
        TreeItem *root = this->rootItem();
        root->forSelectedChildren<BestItem *, Predicate>(pred);
    }

    template <class Predicate>
    void forAllItems(const Predicate &pred) const {
        TreeItem *root = this->rootItem();
        root->forAllChildren<BestItem *, Predicate>(pred);
    }

    BestItem *itemForIndex(const QModelIndex &idx) const {
        return static_cast<BestItem *>(BaseTreeModel::itemForIndex(idx));
    }
};

} // namespace Utils

Q_DECLARE_METATYPE(Utils::TreeItem *)
